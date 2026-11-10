/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* Reused device scratch for search. The SQL path issues one query per call,
   * so allocating and freeing three device buffers for every query is pure
   * overhead; grow-on-demand buffers remove it. Searches on one handle are
   * serialized by the caller, so the buffers need no extra locking. */
  float*    q_buf;
  unsigned* id_buf;
  float*    dist_buf;
  size_t    q_buf_bytes;
  size_t    id_buf_bytes;
  size_t    dist_buf_bytes;
} sk_cuvs_index;

/* ------------------------------------------------------------------------- */
/* Build/search knobs.                                                        */
/*                                                                            */
/* The frozen baseline hard-codes the quality configuration (NN-Descent,      */
/* graph degree 128, intermediate degree 256, 100 iterations). Those exact    */
/* values stay the defaults, so an un-tuned build produces the same graph as  */
/* the baseline, while the knobs are also exposed through the environment so  */
/* that a candidate can be profiled and tuned on the target GPU without       */
/* recompiling the server:                                                    */
/*                                                                            */
/*   SEEKDB_CUVS_BUILD_ALGO           auto | ivf_pq | nn_descent              */
/*   SEEKDB_CUVS_GRAPH_DEGREE         final graph degree                      */
/*   SEEKDB_CUVS_INTERMEDIATE_DEGREE  intermediate graph degree               */
/*   SEEKDB_CUVS_NITER                NN-Descent iterations                   */
/*   SEEKDB_CUVS_ITOPK                search itopk_size (0 = max(512, 4*topk))*/
/*   SEEKDB_CUVS_SEARCH_WIDTH         search_width                            */
/*   SEEKDB_CUVS_TRACE=1              print per-call GPU timings to stderr    */
/* ------------------------------------------------------------------------- */

#define SK_CUVS_DEFAULT_GRAPH_DEGREE        128
#define SK_CUVS_DEFAULT_INTERMEDIATE_DEGREE 256
#define SK_CUVS_DEFAULT_NITER               100

static int sk_env_enabled(const char *name)
{
  const char *v = getenv(name);
  return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
}

static int sk_env_int(const char *name, int fallback, int min_value)
{
  const char *v = getenv(name);
  char *end = NULL;
  long parsed = 0;
  if (v == NULL || v[0] == '\0') return fallback;
  parsed = strtol(v, &end, 10);
  if (end == v || *end != '\0' || parsed < min_value) return fallback;
  return (int)parsed;
}

static int sk_build_algo(void)
{
  const char *v = getenv("SEEKDB_CUVS_BUILD_ALGO");
  if (v == NULL || v[0] == '\0' || strcmp(v, "nn_descent") == 0) return NN_DESCENT;
  if (strcmp(v, "ivf_pq") == 0) return IVF_PQ;
  if (strcmp(v, "auto") == 0) return AUTO_SELECT;
  return NN_DESCENT;
}

static double sk_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  const double t0 = sk_now_ms();
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = sk_build_algo();
  h->ip->graph_degree = sk_env_int("SEEKDB_CUVS_GRAPH_DEGREE", SK_CUVS_DEFAULT_GRAPH_DEGREE, 1);
  h->ip->intermediate_graph_degree =
      sk_env_int("SEEKDB_CUVS_INTERMEDIATE_DEGREE", SK_CUVS_DEFAULT_INTERMEDIATE_DEGREE, 1);
  h->ip->nn_descent_niter = sk_env_int("SEEKDB_CUVS_NITER", SK_CUVS_DEFAULT_NITER, 0);
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  if (sk_env_enabled("SEEKDB_CUVS_TRACE")) {
    fprintf(stderr,
            "[seekdb_cuvs] build n=%ld dim=%ld algo=%d gd=%ld igd=%ld niter=%ld build_ms=%.1f\n",
            n, dim, (int)h->ip->build_algo, (long)h->ip->graph_degree,
            (long)h->ip->intermediate_graph_degree, (long)h->ip->nn_descent_niter,
            sk_now_ms() - t0);
    fflush(stderr);
  }
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  const double t0 = sk_now_ms();
  const size_t q_bytes = sizeof(float) * (size_t)nq * (size_t)h->dim;
  const size_t id_bytes = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  const size_t dist_bytes = sizeof(float) * (size_t)nq * (size_t)topk;
  size_t used_itopk = 0;
  size_t used_width = 0;
  cuvsCagraSearchParams_t sp = 0;
  if (h->q_buf_bytes < q_bytes) {
    if (h->q_buf) { cuvsRMMFree(h->res, h->q_buf, h->q_buf_bytes); h->q_buf = 0; h->q_buf_bytes = 0; }
    if (cuvsRMMAlloc(h->res, (void**)&h->q_buf, q_bytes) != CUVS_SUCCESS) { h->q_buf = 0; rc = 10; goto done; }
    h->q_buf_bytes = q_bytes;
  }
  if (h->id_buf_bytes < id_bytes) {
    if (h->id_buf) { cuvsRMMFree(h->res, h->id_buf, h->id_buf_bytes); h->id_buf = 0; h->id_buf_bytes = 0; }
    if (cuvsRMMAlloc(h->res, (void**)&h->id_buf, id_bytes) != CUVS_SUCCESS) { h->id_buf = 0; rc = 11; goto done; }
    h->id_buf_bytes = id_bytes;
  }
  if (h->dist_buf_bytes < dist_bytes) {
    if (h->dist_buf) { cuvsRMMFree(h->res, h->dist_buf, h->dist_buf_bytes); h->dist_buf = 0; h->dist_buf_bytes = 0; }
    if (cuvsRMMAlloc(h->res, (void**)&h->dist_buf, dist_bytes) != CUVS_SUCCESS) { h->dist_buf = 0; rc = 12; goto done; }
    h->dist_buf_bytes = dist_bytes;
  }
  if (cudaMemcpy(h->q_buf, query, q_bytes, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->q_buf; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->id_buf; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->dist_buf; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc = 14; goto done; }
    {
      const int itopk_override = sk_env_int("SEEKDB_CUVS_ITOPK", 0, 0);
      const int width_override = sk_env_int("SEEKDB_CUVS_SEARCH_WIDTH", 0, 1);
      size_t required_itopk = itopk_override > 0 ? (size_t)itopk_override : (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = width_override > 0 ? (size_t)width_override : 4;
      used_itopk = (size_t)sp->itopk_size;
      used_width = (size_t)sp->search_width;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc = 15; goto done; }
    if (cudaMemcpy(out_ids, h->id_buf, id_bytes, cudaMemcpyDefault) != cudaSuccess) { rc = 16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, h->dist_buf, dist_bytes, cudaMemcpyDefault);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (sk_env_enabled("SEEKDB_CUVS_TRACE")) {
    fprintf(stderr, "[seekdb_cuvs] search nq=%ld topk=%ld itopk=%ld width=%ld rc=%d search_ms=%.2f\n",
            nq, topk, (long)used_itopk, (long)used_width, rc, sk_now_ms() - t0);
    fflush(stderr);
  }
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->q_buf) cuvsRMMFree(h->res, h->q_buf, h->q_buf_bytes);
  if (h->id_buf) cuvsRMMFree(h->res, h->id_buf, h->id_buf_bytes);
  if (h->dist_buf) cuvsRMMFree(h->res, h->dist_buf, h->dist_buf_bytes);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
