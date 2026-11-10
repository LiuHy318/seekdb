/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ob_hipvs_builder.h"

#include <cstring>
#include <cstdlib>
#include <mutex>
#include <dlfcn.h>

// ============================================================================
// Dynamic bindings to the AMD ROCm hipVS (cuVS-compatible) C API.
//
// The hipVS C shared library ships as `libcuvs_c.so` on ROCm systems. We load
// it with dlopen(3) so that seekdb builds and runs unchanged on machines
// without an AMD GPU / ROCm stack: every GPU call degrades gracefully to the
// CPU build path in the caller.
//
// Pipeline used to accelerate HNSW index build:
//   1. cuvsCagraBuild        - GPU builds the KNN (level-0) graph (CAGRA)
//   2. cuvsHnswFromCagra     - CPU builds the upper HNSW hierarchy on top of
//                               the GPU graph (hierarchy = CPU), producing an
//                               hnswlib-compatible index
//   3. cuvsHnswSerialize     - writes the index in hnswlib binary format,
//                               which the caller (vsag) loads back
// ============================================================================

// ---- Opaque handle types (mirror cuVS c_api.h) ---------------------------
typedef void* cuvsResources_t;
typedef void* cuvsCagraIndexParams_t;
typedef void* cuvsCagraIndex_t;
typedef void* cuvsHnswIndexParams_t;
typedef void* cuvsHnswIndex_t;

// cuvsError_t
typedef enum {
  HIPVS_CUVS_ERROR = 0,
  HIPVS_CUVS_SUCCESS = 1,
} hipvs_cuvs_error_t;

// cuvsDistanceType (from cuvs/distance/distance.h)
typedef enum {
  HIPVS_DIST_L2 = 0,            // L2Expanded
  HIPVS_DIST_COSINE = 2,        // CosineExpanded
  HIPVS_DIST_INNER_PRODUCT = 6, // InnerProduct
} hipvs_cuvs_distance_type_t;

// cuvsCagraGraphBuildAlgo
typedef enum {
  HIPVS_CAGRA_AUTO_SELECT = 0,
  HIPVS_CAGRA_IVF_PQ = 1,
  HIPVS_CAGRA_NN_DESCENT = 2,
} hipvs_cagra_graph_build_algo_t;

// cuvsHnswHierarchy
typedef enum {
  HIPVS_HNSW_HIERARCHY_NONE = 0,
  HIPVS_HNSW_HIERARCHY_CPU = 1,
  HIPVS_HNSW_HIERARCHY_GPU = 2,
} hipvs_hnsw_hierarchy_t;

// ---- DLPack DLTensor (subset, enough for CPU float32 2-D tensors) --------
//
// We lay out the structs exactly as DLPack defines them so the ABI matches
// what libcuvs_c.so expects.
typedef enum {
  HIPVS_KDLCPU = 1,
} hipvs_dl_device_type_t;

typedef struct {
  int32_t device_type;
  int32_t device_id;
} hipvs_dl_device_t;

typedef struct {
  uint8_t code;    // kDLFloat = 0
  uint8_t bits;    // 32
  uint16_t lanes;  // 1
} hipvs_dl_dtype_t;

typedef struct {
  void* data;
  hipvs_dl_device_t device;
  int32_t ndim;
  hipvs_dl_dtype_t dtype;
  int64_t* shape;
  int64_t* strides;
  uint64_t byte_offset;
} hipvs_dl_tensor_t;

typedef struct {
  hipvs_dl_tensor_t dl_tensor;
  void* manager_ctx;
  void (*deleter)(void*);
} hipvs_dl_managed_tensor_t;

// ---- Real parameter struct layouts (needed to set fields after Create) ----
typedef struct {
  hipvs_cuvs_distance_type_t metric;
  size_t intermediate_graph_degree;
  size_t graph_degree;
  hipvs_cagra_graph_build_algo_t build_algo;
  size_t nn_descent_niter;
  void* compression;        // cuvsCagraCompressionParams_t
  void* graph_build_params;  // cuvsIvfPqParams_t
} hipvs_cagra_index_params_s;

typedef struct {
  hipvs_hnsw_hierarchy_t hierarchy;
  int ef_construction;
  int num_threads;
} hipvs_hnsw_index_params_s;

// ---- Function pointer types ----------------------------------------------
typedef int (*cuvsResourcesCreate_fn)(cuvsResources_t*);
typedef int (*cuvsResourcesDestroy_fn)(cuvsResources_t);
typedef int (*cuvsCagraIndexParamsCreate_fn)(cuvsCagraIndexParams_t*);
typedef int (*cuvsCagraIndexParamsDestroy_fn)(cuvsCagraIndexParams_t);
typedef int (*cuvsCagraIndexCreate_fn)(cuvsCagraIndex_t*);
typedef int (*cuvsCagraIndexDestroy_fn)(cuvsCagraIndex_t);
typedef int (*cuvsCagraBuild_fn)(cuvsResources_t, cuvsCagraIndexParams_t,
                                 hipvs_dl_managed_tensor_t*, cuvsCagraIndex_t);
typedef int (*cuvsHnswIndexParamsCreate_fn)(cuvsHnswIndexParams_t*);
typedef int (*cuvsHnswIndexParamsDestroy_fn)(cuvsHnswIndexParams_t);
typedef int (*cuvsHnswIndexCreate_fn)(cuvsHnswIndex_t*);
typedef int (*cuvsHnswIndexDestroy_fn)(cuvsHnswIndex_t);
typedef int (*cuvsHnswFromCagra_fn)(cuvsResources_t, cuvsHnswIndexParams_t,
                                    cuvsCagraIndex_t, cuvsHnswIndex_t);
typedef int (*cuvsHnswSerialize_fn)(cuvsResources_t, const char*, cuvsHnswIndex_t);

namespace oceanbase {
namespace common {
namespace obvsag {

static void* hipvs_lib_handle_ = nullptr;
static bool hipvs_checked_ = false;
static bool hipvs_available_ = false;
static std::string hipvs_error_msg_;
static std::mutex hipvs_init_mutex_;

// Loaded function pointers
static cuvsResourcesCreate_fn p_cuvsResourcesCreate = nullptr;
static cuvsResourcesDestroy_fn p_cuvsResourcesDestroy = nullptr;
static cuvsCagraIndexParamsCreate_fn p_cuvsCagraIndexParamsCreate = nullptr;
static cuvsCagraIndexParamsDestroy_fn p_cuvsCagraIndexParamsDestroy = nullptr;
static cuvsCagraIndexCreate_fn p_cuvsCagraIndexCreate = nullptr;
static cuvsCagraIndexDestroy_fn p_cuvsCagraIndexDestroy = nullptr;
static cuvsCagraBuild_fn p_cuvsCagraBuild = nullptr;
static cuvsHnswIndexParamsCreate_fn p_cuvsHnswIndexParamsCreate = nullptr;
static cuvsHnswIndexParamsDestroy_fn p_cuvsHnswIndexParamsDestroy = nullptr;
static cuvsHnswIndexCreate_fn p_cuvsHnswIndexCreate = nullptr;
static cuvsHnswIndexDestroy_fn p_cuvsHnswIndexDestroy = nullptr;
static cuvsHnswFromCagra_fn p_cuvsHnswFromCagra = nullptr;
static cuvsHnswSerialize_fn p_cuvsHnswSerialize = nullptr;

static bool load_hipvs_library() {
  if (hipvs_lib_handle_ != nullptr) {
    return true;
  }

  // hipVS ships the C API as libcuvs_c.so. Try a few candidate names so we
  // tolerate minor packaging differences across ROCm releases.
  static const char* const candidate_names[] = {
      "libcuvs_c.so",
      "libhipvs_c.so",
      "libcuvs_c.so.1",
  };

  for (const char* name : candidate_names) {
    hipvs_lib_handle_ = dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
    if (hipvs_lib_handle_ != nullptr) {
      break;
    }
  }
  if (hipvs_lib_handle_ == nullptr) {
    hipvs_error_msg_ = "dlopen cuvs/hipvs C library failed: " +
                       std::string(dlerror());
    return false;
  }

  // Resolve every required symbol.
  #define RESOLVE(sym) \
    p_##sym = (sym##_fn)dlsym(hipvs_lib_handle_, #sym);

  RESOLVE(cuvsResourcesCreate)
  RESOLVE(cuvsResourcesDestroy)
  RESOLVE(cuvsCagraIndexParamsCreate)
  RESOLVE(cuvsCagraIndexParamsDestroy)
  RESOLVE(cuvsCagraIndexCreate)
  RESOLVE(cuvsCagraIndexDestroy)
  RESOLVE(cuvsCagraBuild)
  RESOLVE(cuvsHnswIndexParamsCreate)
  RESOLVE(cuvsHnswIndexParamsDestroy)
  RESOLVE(cuvsHnswIndexCreate)
  RESOLVE(cuvsHnswIndexDestroy)
  RESOLVE(cuvsHnswFromCagra)
  RESOLVE(cuvsHnswSerialize)
  #undef RESOLVE

  // Verify all symbols resolved.
  if (!p_cuvsResourcesCreate || !p_cuvsResourcesDestroy ||
      !p_cuvsCagraIndexParamsCreate || !p_cuvsCagraIndexParamsDestroy ||
      !p_cuvsCagraIndexCreate || !p_cuvsCagraIndexDestroy ||
      !p_cuvsCagraBuild ||
      !p_cuvsHnswIndexParamsCreate || !p_cuvsHnswIndexParamsDestroy ||
      !p_cuvsHnswIndexCreate || !p_cuvsHnswIndexDestroy ||
      !p_cuvsHnswFromCagra || !p_cuvsHnswSerialize) {
    hipvs_error_msg_ = "failed to resolve required cuvs C API symbols";
    dlclose(hipvs_lib_handle_);
    hipvs_lib_handle_ = nullptr;
    return false;
  }
  return true;
}

int hipvs_init() {
  std::lock_guard<std::mutex> lock(hipvs_init_mutex_);
  if (hipvs_checked_) {
    return hipvs_available_ ? 0 : -1;
  }
  hipvs_checked_ = true;

  if (!load_hipvs_library()) {
    hipvs_available_ = false;
    return -1;
  }

  // Probe GPU availability by creating and immediately destroying resources.
  cuvsResources_t res = nullptr;
  int ret = p_cuvsResourcesCreate(&res);
  if (ret != (int)HIPVS_CUVS_SUCCESS || res == nullptr) {
    hipvs_error_msg_ = "cuvsResourcesCreate failed (no AMD ROCm GPU?)";
    hipvs_available_ = false;
    return -1;
  }
  p_cuvsResourcesDestroy(res);
  hipvs_available_ = true;
  return 0;
}

bool hipvs_is_available() {
  std::lock_guard<std::mutex> lock(hipvs_init_mutex_);
  if (!hipvs_checked_) {
    hipvs_init();
  }
  return hipvs_available_;
}

void hipvs_destroy() {
  std::lock_guard<std::mutex> lock(hipvs_init_mutex_);
  if (hipvs_lib_handle_ != nullptr) {
    dlclose(hipvs_lib_handle_);
    hipvs_lib_handle_ = nullptr;
  }
  hipvs_available_ = false;
  hipvs_checked_ = false;
}

static hipvs_cuvs_distance_type_t map_metric(HipvsMetricType m) {
  switch (m) {
    case HIPVS_METRIC_INNER_PRODUCT: return HIPVS_DIST_INNER_PRODUCT;
    case HIPVS_METRIC_COSINE:        return HIPVS_DIST_COSINE;
    case HIPVS_METRIC_L2:
    default:                         return HIPVS_DIST_L2;
  }
}

int hipvs_build_hnsw_gpu(
    const float* vectors,
    const int64_t* /*ids*/,
    const HipvsBuildParams& params,
    const std::string& output_file) {

  if (!hipvs_available_ || vectors == nullptr || params.dim <= 0 ||
      params.num_vectors <= 0) {
    return -1;
  }

  int ret = -1;
  cuvsResources_t res = nullptr;
  cuvsCagraIndexParams_t cagra_params = nullptr;
  cuvsCagraIndex_t cagra_index = nullptr;
  cuvsHnswIndexParams_t hnsw_params = nullptr;
  cuvsHnswIndex_t hnsw_index = nullptr;

  // DLTensor shape (must outlive the build call)
  int64_t shape[2] = {params.num_vectors, params.dim};

  do {
    // 1. Resources
    if (p_cuvsResourcesCreate(&res) != HIPVS_CUVS_SUCCESS) {
      hipvs_error_msg_ = "cuvsResourcesCreate failed";
      break;
    }

    // 2. CAGRA build params
    if (p_cuvsCagraIndexParamsCreate(&cagra_params) != HIPVS_CUVS_SUCCESS) {
      hipvs_error_msg_ = "cuvsCagraIndexParamsCreate failed";
      break;
    }
    {
      auto* p = reinterpret_cast<hipvs_cagra_index_params_s*>(cagra_params);
      p->metric = map_metric(params.metric);
      // graph_degree = HNSW M; intermediate degree is typically 2*M.
      size_t m = (params.max_degree > 0) ? (size_t)params.max_degree : 16;
      p->graph_degree = m;
      p->intermediate_graph_degree = 2 * m;
      p->build_algo = HIPVS_CAGRA_AUTO_SELECT;
      p->nn_descent_niter = 20;
    }

    // 3. CAGRA index handle
    if (p_cuvsCagraIndexCreate(&cagra_index) != HIPVS_CUVS_SUCCESS) {
      hipvs_error_msg_ = "cuvsCagraIndexCreate failed";
      break;
    }

    // 4. Build the dataset tensor (CPU float32, row-major).
    hipvs_dl_managed_tensor_t dataset{};
    dataset.dl_tensor.data = const_cast<float*>(vectors);
    dataset.dl_tensor.device.device_type = HIPVS_KDLCPU;
    dataset.dl_tensor.device.device_id = 0;
    dataset.dl_tensor.ndim = 2;
    dataset.dl_tensor.dtype.code = 0;    // kDLFloat
    dataset.dl_tensor.dtype.bits = 32;
    dataset.dl_tensor.dtype.lanes = 1;
    dataset.dl_tensor.shape = shape;
    dataset.dl_tensor.strides = nullptr;
    dataset.dl_tensor.byte_offset = 0;
    dataset.manager_ctx = nullptr;
    dataset.deleter = nullptr;

    // 5. GPU CAGRA build - this is the accelerated step.
    if (p_cuvsCagraBuild(res, cagra_params, &dataset, cagra_index) !=
        HIPVS_CUVS_SUCCESS) {
      hipvs_error_msg_ = "cuvsCagraBuild failed";
      break;
    }

    // 6. HNSW params: build the upper hierarchy on CPU so the serialized
    //    index is hnswlib-compatible and loadable by vsag.
    if (p_cuvsHnswIndexParamsCreate(&hnsw_params) != HIPVS_CUVS_SUCCESS) {
      hipvs_error_msg_ = "cuvsHnswIndexParamsCreate failed";
      break;
    }
    {
      auto* p = reinterpret_cast<hipvs_hnsw_index_params_s*>(hnsw_params);
      p->hierarchy = HIPVS_HNSW_HIERARCHY_CPU;
      p->ef_construction = (params.ef_construction > 0)
                                ? params.ef_construction
                                : 200;
      p->num_threads = 0;  // 0 = auto
    }

    // 7. HNSW index handle
    if (p_cuvsHnswIndexCreate(&hnsw_index) != HIPVS_CUVS_SUCCESS) {
      hipvs_error_msg_ = "cuvsHnswIndexCreate failed";
      break;
    }

    // 8. Convert CAGRA (GPU graph) -> HNSW (adds CPU hierarchy)
    if (p_cuvsHnswFromCagra(res, hnsw_params, cagra_index, hnsw_index) !=
        HIPVS_CUVS_SUCCESS) {
      hipvs_error_msg_ = "cuvsHnswFromCagra failed";
      break;
    }

    // 9. Serialize to hnswlib binary format.
    if (p_cuvsHnswSerialize(res, output_file.c_str(), hnsw_index) !=
        HIPVS_CUVS_SUCCESS) {
      hipvs_error_msg_ = "cuvsHnswSerialize failed";
      break;
    }

    ret = 0;
  } while (0);

  // Cleanup
  if (hnsw_index) p_cuvsHnswIndexDestroy(hnsw_index);
  if (hnsw_params) p_cuvsHnswIndexParamsDestroy(hnsw_params);
  if (cagra_index) p_cuvsCagraIndexDestroy(cagra_index);
  if (cagra_params) p_cuvsCagraIndexParamsDestroy(cagra_params);
  if (res) p_cuvsResourcesDestroy(res);

  return ret;
}

const char* hipvs_last_error() {
  return hipvs_error_msg_.c_str();
}

}  // namespace obvsag
}  // namespace common
}  // namespace oceanbase
