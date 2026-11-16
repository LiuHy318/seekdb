# seekdb full rebuild on the Radeon Cloud instance — state and recipe

This file records the working recipe for compiling the frozen contest baseline (`634888d9`) on the
`rgapi-hf-*` Radeon Cloud instance (gfx1100 / ROCm 7.2.4), plus the exact blockers hit so a later
session can resume instead of rediscovering them. It lives next to the source tarball on the
`src-snapshot` branch and is never part of a submission branch.

## Why a full rebuild at all

The timed window is `CREATE VECTOR INDEX` -> index queryable -> first fixed query returns.
Scoring is `4 + 14*log3(build_ratio)` and the two query points need `query_ratio >= 1.0`.
v8 reached `build=1.373x`, i.e. 8.043 points, with the bridge already at its ceiling:
the window contains ~13 s that no `.so` can touch (row scan / snapshot write / snapshot read).
Attack surface left is server-side C++, so `src/oblib/lib/vector/ob_vsag_adaptor.cpp` must be
edited and seekdb recompiled.

## Verified recipe (in order)

1. Source delivery. GitHub HTTPS from the instance runs at ~47 KB/s, so a trimmed tarball is
   pushed from the workstation and fetched with git:
   `git fetch origin src-snapshot --depth 1 && git show FETCH_HEAD:seekdb_src.tgz > /workspace/seekdb_src.tgz`
   `tar -xzf /workspace/seekdb_src.tgz -C /workspace/src_full` (19.6 MB, unpacks ~300 MB).
2. Line endings. The tarball is produced from a Windows checkout and carries CRLF, which breaks
   every shell script (`set: pipefail`, `$'\r': command not found`). Fix once per tree:
   `find . -name '*.sh' -print0 | xargs -0 sed -i 's/\r$//'`
3. Bridge. `HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm bash tools/hipvs_bridge/build.sh <out>.so`
   (needs the CRLF fix first). Produces a working `libseekdb_cuvs_bridge.so`.
4. Toolchain. `clang`/`ld.lld` are absent from `/usr/bin`; the ROCm bundle has them:
   `export PATH=/opt/rocm/llvm/bin:/workspace/src_full/deps/3rd/usr/local/oceanbase/devtools/bin:$PATH`
   (`bison` and `flex` also live in that devtools bin and the SQL parser generation needs them).
5. Dependencies. `deps/init/dep_create.sh` cannot resolve its cache key for a trimmed tree.
   Symlink the cached tree instead:
   `ln -sfn /opt/seekdb-deps-cache/ac032efc6b707fc0273e6b553db53011/3rd /workspace/src_full/deps/3rd`
6. Configure. `-DOB_CC`/`-DOB_CXX` must be passed explicitly, otherwise `cmake/Env.cmake:394`
   aborts with "can't find suitable compiler":
   ```
   cmake -S . -B build_release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
     -DOB_CC=/opt/rocm/llvm/bin/clang -DOB_CXX=/opt/rocm/llvm/bin/clang++ \
     -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
     -DCUVS_BRIDGE_LIB=/workspace/br2/libseekdb_cuvs_bridge.so
   ```
7. Build. `cmake --build build_release --target seekdb -- -j96`.

## Trimmed-tree patches (build-only, never committed)

The official tree is not trimmed, so these only exist to make a 19.6 MB package buildable:

- `unittest/CMakeLists.txt` must exist (`printf '# excluded\n' > unittest/CMakeLists.txt`), and
  `unittest/<module>/BUILD.bazel` for oblib share storage sql rootserver observer logservice pl
  query data_plane are required by `tools/cmake/emit_bazel_source_inventory.py`.
- That script also checks that every Unity-exception source exists; skip the check with
  `sed -i 's#if not (repo / path).is_file()#if False#' tools/cmake/emit_bazel_source_inventory.py`.
- `CMakeLists.txt` installs `$<TARGET_FILE:xxx_tests>`, which fails when the tests are missing;
  guard it with `if(TARGET ${SEEKDB_PRETEST_TARGET}) ... endif()`.
- `src/oblib/CMakeLists.txt` passes `-Werror`; AMD clang 22 rejects
  `-Wno-enum-constexpr-conversion` (unknown warning) and the whole code base trips newer
  `-Wunnecessary-virtual-specifier` / `-Wcast-function-type-mismatch`. Replace `-Werror` with
  `-Wno-error` in that file (a warm build then goes 0 -> 97 % in about 20 minutes at -j96).

## Remaining blocker (as of 2026-11-17 03:00)

40 errors left, all from `boost/mpl/aux_/integral_wrapper.hpp` ("non-type template argument is
not a constant expression") in `src/share/geo/ob_geo_func_{equals,overlaps,touches,distance}*.cpp`
and a few unity batches of `ob_server` / `ob_rootserver`. `prior`/`next` were rewritten to use
`N`'s literal form and it still trips, so clang 22 is rejecting the recursive
`AUX_WRAPPER_NAME<T, static_cast<T>(N-1)>` instantiation itself.

Next attempt, in order of cost:

1. Compile the whole tree with gcc 13 instead of clang 22 (Env.cmake only warns on the unsupported
   gcc path; pass `-DOB_USE_CLANG=OFF -DOB_CC=/usr/bin/gcc -DOB_CXX=/usr/bin/g++`). Costs one full
   rebuild (~20-30 min), avoids every clang-22-only diagnostic.
2. Or teach the boost header to stop instantiating `prior`/`next` eagerly (defer them behind a
   template so clang never forms the dependent argument).
3. Or keep clang for everything except those ~40 translation units (mixing clang++ and g++ objects
   is ABI-safe here, both target the Itanium C++ ABI with libstdc++).

## What still has to happen after the first successful build

1. Confirm the freshly built `seekdb` reproduces the v8 window on `/workspace/bench/base.f32`
   (`x_bench.py`, `BN=250000 BDIM=128`), using the template's observer as the control.
2. Add timing instrumentation around `ob_cuvs_register`, `ob_cuvs_serialize`,
   `ob_cuvs_deserialize` and the DDL row scan to replace the current 13 s estimate with real
   segment numbers.
3. Optimize the segments that dominate (drop the duplicate 128 MB `buf_vecs_` copy,
   `include_dataset=false` plus rebind at load, remove the tmpfile double write).
4. Re-measure, submit a fresh branch, and only then update `SUBMISSION.md`.
