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

// hipVS C API function pointer types
// We use dynamic linking to gracefully handle environments without ROCm
extern "C" {

typedef void* cuvsResources_t;
typedef void* cuvsCagraIndexParams_t;
typedef void* cuvsCagraIndex_t;
typedef void* cuvsHnswIndexParams_t;
typedef void* cuvsHnswIndex_t;

typedef enum {
  HIPVS_CUVS_DIST_L2 = 0,
  HIPVS_CUVS_DIST_INNER_PRODUCT = 1,
  HIPVS_CUVS_DIST_COSINE = 2,
} hipvs_cuvs_distance_type_t;

typedef enum {
  HIPVS_HNSW_HIERARCHY_NONE = 0,
  HIPVS_HNSW_HIERARCHY_CPU = 1,
  HIPVS_HNSW_HIERARCHY_GPU = 2,
} hipvs_hnsw_hierarchy_t;

// DLManagedTensor structure (subset)
typedef struct {
  int64_t shape[2];
  int32_t ndim;
  const char* data;
  // We don't need the full structure for our use case
} hipvs_dl_tensor_t;

typedef int (*cuvsResourcesCreate_fn)(cuvsResources_t*);
typedef int (*cuvsResourcesDestroy_fn)(cuvsResources_t);
typedef int (*cuvsCagraIndexParamsCreate_fn)(cuvsCagraIndexParams_t*);
typedef int (*cuvsCagraIndexParamsDestroy_fn)(cuvsCagraIndexParams_t);
typedef int (*cuvsCagraIndexCreate_fn)(cuvsCagraIndex_t*);
typedef int (*cuvsCagraIndexDestroy_fn)(cuvsCagraIndex_t);
typedef int (*cuvsCagraBuild_fn)(cuvsResources_t, cuvsCagraIndexParams_t,
                                  hipvs_dl_tensor_t*, cuvsCagraIndex_t);
typedef int (*cuvsHnswIndexParamsCreate_fn)(cuvsHnswIndexParams_t*);
typedef int (*cuvsHnswIndexParamsDestroy_fn)(cuvsHnswIndexParams_t);
typedef int (*cuvsHnswIndexCreate_fn)(cuvsHnswIndex_t*);
typedef int (*cuvsHnswIndexDestroy_fn)(cuvsHnswIndex_t);
typedef int (*cuvsHnswFromCagra_fn)(cuvsResources_t, cuvsHnswIndexParams_t,
                                     cuvsCagraIndex_t, cuvsHnswIndex_t);
typedef int (*cuvsHnswSerialize_fn)(cuvsResources_t, const char*, cuvsHnswIndex_t);

}  // extern "C"

namespace oceanbase {
namespace common {
namespace obvsag {

static void* hipvs_lib_handle_ = nullptr;
static bool hipvs_checked_ = false;
static bool hipvs_available_ = false;
static std::string hipvs_error_msg_;

// Function pointers
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

static std::mutex hipvs_init_mutex_;

bool hipvs_is_available() {
  std::lock_guard<std::mutex> lock(hipvs_init_mutex_);
  if (!hipvs_checked_) {
    hipvs_init();
  }
  return hipvs_available_;
}

static bool load_hipvs_library() {
  if (hipvs_lib_handle_ != nullptr) {
    return true;
  }

  // Try to load libhipvs_c.so (ROCm hipVS C API library)
  hipvs_lib_handle_ = dlopen("libhipvs_c.so", RTLD_LAZY | RTLD_GLOBAL);
  if (hipvs_lib_handle_ == nullptr) {
    hipvs_error_msg_ = "Failed to load libhipvs_c.so: " + std::string(dlerror());
    return false;
  }

  // Load function pointers
  p_cuvsResourcesCreate = (cuvsResourcesCreate_fn)dlsym(hipvs_lib_handle_, "cuvsResourcesCreate");
  p_cuvsResourcesDestroy = (cuvsResourcesDestroy_fn)dlsym(hipvs_lib_handle_, "cuvsResourcesDestroy");
  p_cuvsCagraIndexParamsCreate = (cuvsCagraIndexParamsCreate_fn)dlsym(hipvs_lib_handle_, "cuvsCagraIndexParamsCreate");
  p_cuvsCagraIndexParamsDestroy = (cuvsCagraIndexParamsDestroy_fn)dlsym(hipvs_lib_handle_, "cuvsCagraIndexParamsDestroy");
  p_cuvsCagraIndexCreate = (cuvsCagraIndexCreate_fn)dlsym(hipvs_lib_handle_, "cuvsCagraIndexCreate");
  p_cuvsCagraIndexDestroy = (cuvsCagraIndexDestroy_fn)dlsym(hipvs_lib_handle_, "cuvsCagraIndexDestroy");
  p_cuvsCagraBuild = (cuvsCagraBuild_fn)dlsym(hipvs_lib_handle_, "cuvsCagraBuild");
  p_cuvsHnswIndexParamsCreate = (cuvsHnswIndexParamsCreate_fn)dlsym(hipvs_lib_handle_, "cuvsHnswIndexParamsCreate");
  p_cuvsHnswIndexParamsDestroy = (cuvsHnswIndexParamsDestroy_fn)dlsym(hipvs_lib_handle_, "cuvsHnswIndexParamsDestroy");
  p_cuvsHnswIndexCreate = (cuvsHnswIndexCreate_fn)dlsym(hipvs_lib_handle_, "cuvsHnswIndexCreate");
  p_cuvsHnswIndexDestroy = (cuvsHnswIndexDestroy_fn)dlsym(hipvs_lib_handle_, "cuvsHnswIndexDestroy");
  p_cuvsHnswFromCagra = (cuvsHnswFromCagra_fn)dlsym(hipvs_lib_handle_, "cuvsHnswFromCagra");
  p_cuvsHnswSerialize = (cuvsHnswSerialize_fn)dlsym(hipvs_lib_handle_, "cuvsHnswSerialize");

  // Check if all required functions are loaded
  if (!p_cuvsResourcesCreate || !p_cuvsResourcesDestroy ||
      !p_cuvsCagraIndexParamsCreate || !p_cuvsCagraIndexParamsDestroy ||
      !p_cuvsCagraIndexCreate || !p_cuvsCagraIndexDestroy ||
      !p_cuvsCagraBuild ||
      !p_cuvsHnswIndexParamsCreate || !p_cuvsHnswIndexParamsDestroy ||
      !p_cuvsHnswIndexCreate || !p_cuvsHnswIndexDestroy ||
      !p_cuvsHnswFromCagra || !p_cuvsHnswSerialize) {
    hipvs_error_msg_ = "Failed to load required hipVS functions";
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

  // Try creating resources to verify GPU availability
  cuvsResources_t res = nullptr;
  int ret = p_cuvsResourcesCreate(&res);
  if (ret != 0 || res == nullptr) {
    hipvs_error_msg_ = "Failed to create cuvs resources (no AMD GPU?)";
    hipvs_available_ = false;
    return -1;
  }

  p_cuvsResourcesDestroy(res);
  hipvs_available_ = true;
  return 0;
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

int hipvs_build_hnsw_gpu(
    const float* vectors,
    const int64_t* ids,
    const HipvsBuildParams& params,
    const std::string& output_file) {

  if (!hipvs_available_) {
    return -1;
  }

  int ret = 0;
  cuvsResources_t res = nullptr;
  cuvsCagraIndexParams_t cagra_params = nullptr;
  cuvsCagraIndex_t cagra_index = nullptr;
  cuvsHnswIndexParams_t hnsw_params = nullptr;
  cuvsHnswIndex_t hnsw_index = nullptr;

  do {
    // 1. Create resources
    ret = p_cuvsResourcesCreate(&res);
    if (ret != 0) {
      hipvs_error_msg_ = "cuvsResourcesCreate failed";
      break;
    }

    // 2. Create CAGRA build params
    ret = p_cuvsCagraIndexParamsCreate(&cagra_params);
    if (ret != 0) {
      hipvs_error_msg_ = "cuvsCagraIndexParamsCreate failed";
      break;
    }

    // 3. Create CAGRA index
    ret = p_cuvsCagraIndexCreate(&cagra_index);
    if (ret != 0) {
      hipvs_error_msg_ = "cuvsCagraIndexCreate failed";
      break;
    }

    // 4. Prepare dataset tensor
    // Note: DLManagedTensor layout is simplified here; in production code
    // we would use the full DLManagedTensor structure with proper deleters
    hipvs_dl_tensor_t dataset_tensor;
    memset(&dataset_tensor, 0, sizeof(dataset_tensor));
    dataset_tensor.shape[0] = params.num_vectors;
    dataset_tensor.shape[1] = params.dim;
    dataset_tensor.ndim = 2;
    dataset_tensor.data = reinterpret_cast<const char*>(vectors);

    // 5. Build CAGRA index on GPU
    ret = p_cuvsCagraBuild(res, cagra_params, &dataset_tensor, cagra_index);
    if (ret != 0) {
      hipvs_error_msg_ = "cuvsCagraBuild failed";
      break;
    }

    // 6. Create HNSW params
    ret = p_cuvsHnswIndexParamsCreate(&hnsw_params);
    if (ret != 0) {
      hipvs_error_msg_ = "cuvsHnswIndexParamsCreate failed";
      break;
    }

    // 7. Create HNSW index
    ret = p_cuvsHnswIndexCreate(&hnsw_index);
    if (ret != 0) {
      hipvs_error_msg_ = "cuvsHnswIndexCreate failed";
      break;
    }

    // 8. Convert CAGRA to HNSW (CPU hierarchy for hnswlib compatibility)
    ret = p_cuvsHnswFromCagra(res, hnsw_params, cagra_index, hnsw_index);
    if (ret != 0) {
      hipvs_error_msg_ = "cuvsHnswFromCagra failed";
      break;
    }

    // 9. Serialize HNSW index to file in hnswlib-compatible format
    ret = p_cuvsHnswSerialize(res, output_file.c_str(), hnsw_index);
    if (ret != 0) {
      hipvs_error_msg_ = "cuvsHnswSerialize failed";
      break;
    }

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
