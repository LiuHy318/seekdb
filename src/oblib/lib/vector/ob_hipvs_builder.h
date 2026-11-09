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

#pragma once

#include <stdint.h>
#include <string>

namespace oceanbase {
namespace common {
namespace obvsag {

// hipVS GPU accelerated HNSW builder
// This module uses AMD ROCm hipVS library to accelerate HNSW index build
// by leveraging GPU-accelerated graph construction (CAGRA) and then
// converting the result to a CPU-searchable HNSW index.

enum HipvsMetricType {
  HIPVS_METRIC_L2 = 0,
  HIPVS_METRIC_INNER_PRODUCT = 1,
  HIPVS_METRIC_COSINE = 2,
};

struct HipvsBuildParams {
  int dim = 0;
  int num_vectors = 0;
  int max_degree = 0;       // HNSW M parameter
  int ef_construction = 0;  // HNSW efConstruction
  HipvsMetricType metric = HIPVS_METRIC_L2;
  bool use_gpu_hierarchy = false;  // true: GPU build hierarchy, false: CPU build hierarchy
};

// Check if hipVS is available (ROCm GPU present and library loaded)
bool hipvs_is_available();

// Initialize hipVS runtime
// Returns 0 on success, negative error code on failure
int hipvs_init();

// Finalize hipVS runtime
void hipvs_destroy();

// Build HNSW index using GPU acceleration via hipVS CAGRA
//
// @param vectors: flat float array of shape [num_vectors, dim], row-major
// @param ids: array of vector ids, shape [num_vectors]
// @param params: build parameters
// @param output_file: path to save the built index in hnswlib-compatible format
//
// @return 0 on success, negative error code on failure
int hipvs_build_hnsw_gpu(
    const float* vectors,
    const int64_t* ids,
    const HipvsBuildParams& params,
    const std::string& output_file);

// Get last error message
const char* hipvs_last_error();

}  // namespace obvsag
}  // namespace common
}  // namespace oceanbase
