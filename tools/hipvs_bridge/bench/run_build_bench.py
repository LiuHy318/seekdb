#!/usr/bin/env python3
"""End-to-end index-build benchmark for the seekdb hipVS/cuVS backend.

The contest scores the offline-DDL path:

    CREATE VECTOR INDEX idx_items ON items(v)
      WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);

and the timed window runs from that statement until the index is queryable *and*
the first fixed query has returned. This script reproduces exactly that window on
a single node:

  * start a private seekdb observer (own base-dir / port / data-dir);
  * create `items(id, v)` and load `n` float32 rows;
  * time `CREATE VECTOR INDEX` + the first `ORDER BY l2_distance(...) APPROXIMATE`
    query on the SAME mysql session (so the first query is charged to the build);
  * run `nq` fixed Top-k queries and report QPS / average latency;
  * compute Recall@k against an exact NumPy ground truth;
  * optionally repeat the build with `lib=vsag` to produce the CPU/GPU comparison.

Every CAGRA build/search knob of the GPU bridge can be overridden with the
`--param NAME=VALUE` option (the runner exports the knob to the observer):

    --param SEEKDB_CUVS_BUILD_ALGO=nn_descent
    --param SEEKDB_CUVS_GRAPH_DEGREE=128
    --param SEEKDB_CUVS_INTERMEDIATE_DEGREE=256
    --param SEEKDB_CUVS_NITER=100
    --param SEEKDB_CUVS_ITOPK=0 --param SEEKDB_CUVS_SEARCH_WIDTH=4

`SEEKDB_CUVS_TRACE=1` additionally makes the bridge print per-call GPU timings.

Example (250K x 128, SIFT-like synthetic data):

    python3 tools/hipvs_bridge/bench/run_build_bench.py \
      --observer build_release/src/observer/seekdb \
      --bridge /work/bridge/libseekdb_cuvs_bridge.so \
      --base-dir /tmp/seekdb-bench --port 2981 \
      --n 250000 --dim 128 --nq 256 \
      --out /tmp/seekdb-bench/evidence

Result files: `results.json` (machine readable), `results.md` (report row),
`observer.log` (server log, contains the `[OBVSAG][cuVS] serialized GPU CAGRA
snapshot ... build_us=...` line), and `bridge-trace.log` when tracing is on.
"""

import argparse
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import time

import numpy as np

MYSQL = "mysql"
OBSERVER_ENV_KEYS = (
    "SEEKDB_CUVS_BUILD_ALGO",
    "SEEKDB_CUVS_GRAPH_DEGREE",
    "SEEKDB_CUVS_INTERMEDIATE_DEGREE",
    "SEEKDB_CUVS_NITER",
    "SEEKDB_CUVS_ITOPK",
    "SEEKDB_CUVS_SEARCH_WIDTH",
    "SEEKDB_CUVS_TRACE",
    "HIP_VISIBLE_DEVICES",
    "ROCR_VISIBLE_DEVICES",
)


def log(message):
    print(f"[bench] {message}", flush=True)


def mysql(port, sql, timeout=600, check=True, local_infile=False):
    cmd = [MYSQL, "--connect-timeout=5", "--protocol=TCP", "-h127.0.0.1",
           f"-P{port}", "-uroot", "-A", "-N", "-B"]
    if local_infile:
        cmd.append("--local-infile=1")
    cmd += ["-e", sql]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=check)


def mysql_file(port, path, timeout=3600, check=True):
    cmd = [MYSQL, "--connect-timeout=5", "--protocol=TCP", "-h127.0.0.1",
           f"-P{port}", "-uroot", "-A", "-N", "-B"]
    with open(path, "r", encoding="utf-8") as handle:
        return subprocess.run(cmd, stdin=handle, capture_output=True, text=True,
                              timeout=timeout, check=check)


def wait_ready(port, proc, timeout=180.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            if mysql(port, "select 1", timeout=10, check=False).returncode == 0:
                return True
        except subprocess.TimeoutExpired:
            pass
        time.sleep(1.0)
    return False


def port_free(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.5)
        return sock.connect_ex(("127.0.0.1", port)) != 0


class Observer:
    def __init__(self, args, run_dir, port):
        self.args = args
        self.run_dir = run_dir
        self.port = port
        self.proc = None

    def __enter__(self):
        env = dict(os.environ)
        bridge_dir = str(pathlib.Path(self.args.bridge).resolve().parent)
        env["LD_LIBRARY_PATH"] = ":".join(
            [bridge_dir, "/opt/hipvs/lib", "/opt/rocm/lib",
             env.get("LD_LIBRARY_PATH", "")]).strip(":")
        env["SEEKDB_CUVS_TRACE"] = self.args.trace and "1" or "0"
        for key, value in self.args.params.items():
            env[key] = str(value)
        store = self.run_dir / "store"
        store.mkdir(parents=True, exist_ok=True)
        self.log_path = self.args.out / "observer.log"
        cmd = [
            str(self.args.observer),
            "--nodaemon",
            "--port", str(self.port),
            "--data-dir", str(store),
            "--base-dir", str(self.run_dir),
            "--parameter", "__min_full_resource_pool_memory=2147483648",
            "--parameter", "memory_budget=2G",
            "--parameter", f"datafile_size={self.args.datafile_size}",
            "--parameter", f"cpu_count={self.args.cpu_count}",
            "--parameter", "local_ip=127.0.0.1",
        ]
        log(f"starting observer: port={self.port} base_dir={self.run_dir}")
        with open(self.log_path, "w", encoding="utf-8") as handle:
            self.proc = subprocess.Popen(cmd, env=env, stdout=handle,
                                         stderr=subprocess.STDOUT,
                                         start_new_session=True)
        if not wait_ready(self.port, self.proc, timeout=self.args.start_timeout):
            raise RuntimeError(f"observer did not become SQL-ready, see {self.log_path}")
        log("observer ready")
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.proc is None or self.proc.poll() is not None:
            return
        log("stopping observer")
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            self.proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=30)


def read_fvecs(path, limit):
    raw = np.fromfile(path, dtype=np.int32)
    dim = int(raw[0])
    stride = dim + 1
    count = min(limit, raw.size // stride)
    data = raw[: count * stride].reshape(count, stride)[:, 1:].copy()
    return data.view(np.float32), dim


def make_dataset(args):
    if args.fvecs:
        vectors, dim = read_fvecs(args.fvecs, args.n)
        if dim != args.dim:
            raise SystemExit(f"{args.fvecs} is {dim}-d but --dim={args.dim}")
        log(f"loaded {vectors.shape[0]} vectors from {args.fvecs}")
        vectors = np.ascontiguousarray(vectors, dtype=np.float32)
    else:
        rng = np.random.default_rng(args.seed)
        clusters = 256
        centers = rng.normal(size=(clusters, args.dim)).astype(np.float32) * 2.0
        assign = rng.integers(0, clusters, size=args.n)
        vectors = centers[assign] + rng.normal(
            scale=0.25, size=(args.n, args.dim)).astype(np.float32)
        log(f"generated {args.n} synthetic {args.dim}-d vectors ({clusters} clusters)")

    rng = np.random.default_rng(args.seed + 1)
    # Half the probes are perturbed base rows (SIFT-style), half are fresh points.
    half = args.nq // 2
    rows = rng.integers(0, vectors.shape[0], size=half)
    queries = vectors[rows] + rng.normal(
        scale=0.02, size=(half, args.dim)).astype(np.float32)
    queries = np.concatenate([
        queries,
        rng.normal(scale=1.0, size=(args.nq - half, args.dim)).astype(np.float32)])
    return vectors, queries


def exact_ground_truth(vectors, queries, ks):
    """Exact L2 top-m ax ids; m = max(ks). Chunked to bound memory."""
    m = max(ks)
    out = np.empty((queries.shape[0], m), dtype=np.int64)
    sq = np.sum(queries ** 2, axis=1)[:, None]
    for start in range(0, queries.shape[0], 32):
        chunk = queries[start:start + 32]
        dist = sq[start:start + 32] + np.sum(vectors ** 2, axis=1)[None, :] - 2.0 * chunk @ vectors.T
        out[start:start + 32] = np.argpartition(dist, m - 1, axis=1)[:, :m]
    return out


def vector_literal(values):
    return "[" + ",".join(f"{v:.7g}" for v in values) + "]"


def write_insert_sql(path, table, vectors, chunk):
    ids = np.arange(vectors.shape[0], dtype=np.int64)
    with open(path, "w", encoding="utf-8") as sql:
        sql.write("use bench;\n")
        for start in range(0, vectors.shape[0], chunk):
            rows = [f"({int(row_id)},'{vector_literal(vector)}')"
                    for row_id, vector in zip(ids[start:start + chunk],
                                              vectors[start:start + chunk])]
            sql.write(f"insert into {table} values " + ",".join(rows) + ";\n")


def write_query_sql(path, limit, queries):
    with open(path, "w", encoding="utf-8") as sql:
        sql.write("use bench;\n")
        for query in queries:
            sql.write("select id from items order by "
                      f"l2_distance(v,'{vector_literal(query)}') APPROXIMATE "
                      f"limit {limit};\n")


def parse_ids(stdout, nq, k):
    rows = [line.strip() for line in stdout.splitlines() if line.strip()]
    if len(rows) != nq * k:
        raise RuntimeError(f"expected {nq * k} id rows, got {len(rows)}")
    values = np.array([int(row) for row in rows], dtype=np.int64)
    return values.reshape(nq, k)


def recall(returned, truth, k):
    hits = 0
    for row, exact in zip(returned, truth):
        hits += len(np.intersect1d(row[:k], exact[:k], assume_unique=True))
    return hits / (returned.shape[0] * k)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--observer", required=True)
    parser.add_argument("--bridge", required=True)
    parser.add_argument("--base-dir", required=True)
    parser.add_argument("--port", type=int, default=2981)
    parser.add_argument("--out", required=True)
    parser.add_argument("--n", type=int, default=250_000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--nq", type=int, default=256)
    parser.add_argument("--topk", default="10,100")
    parser.add_argument("--fvecs", default="", help="optional .fvecs dataset (SIFT)")
    parser.add_argument("--seed", type=int, default=1314)
    parser.add_argument("--insert-chunk", type=int, default=500)
    parser.add_argument("--lib", default="cuvs", choices=("cuvs", "vsag", "both"))
    parser.add_argument("--param", action="append", default=[],
                        metavar="KEY=VALUE", help="extra observer environment")
    parser.add_argument("--trace", action="store_true",
                        help="enable SEEKDB_CUVS_TRACE in the observer")
    parser.add_argument("--start-timeout", type=float, default=240.0)
    parser.add_argument("--datafile-size", default="20G")
    parser.add_argument("--cpu-count", default="24")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    args.out = pathlib.Path(args.out)
    args.out.mkdir(parents=True, exist_ok=True)
    args.base_dir = pathlib.Path(args.base_dir)
    args.params = {}
    for item in args.param:
        if "=" not in item:
            raise SystemExit(f"--param expects KEY=VALUE, got {item}")
        key, value = item.split("=", 1)
        args.params[key.strip()] = value.strip()

    if shutil.which(MYSQL) is None:
        raise SystemExit(f"{MYSQL} client not found in PATH")
    if not port_free(args.port):
        raise SystemExit(f"port {args.port} is already in use")

    ks = [int(value) for value in args.topk.split(",") if value]
    vectors, queries = make_dataset(args)
    log("computing exact ground truth")
    truth = exact_ground_truth(vectors, queries, ks)
    log(f"ground truth ready: nq={queries.shape[0]} ks={ks}")

    libs = ["cuvs", "vsag"] if args.lib == "both" else [args.lib]
    results = {
        "config": {
            "n": int(vectors.shape[0]),
            "dim": int(vectors.shape[1]),
            "nq": int(queries.shape[0]),
            "topk": ks,
            "params": args.params,
            "bridge": str(args.bridge),
            "observer": str(args.observer),
        },
        "runs": [],
    }

    run_dir = args.base_dir / f"run-{int(time.time())}"
    shutil.rmtree(run_dir, ignore_errors=True)
    run_dir.mkdir(parents=True, exist_ok=True)

    with Observer(args, run_dir, args.port) as observer:
        mysql(args.port, "create database if not exists bench;")
        mysql(args.port, "alter system set ob_vector_memory_limit_percentage=30;")
        mysql(args.port, f"use bench; create table items(id bigint primary key, v vector({args.dim}));")

        load_sql = args.out / "load.sql"
        log(f"loading {vectors.shape[0]} rows")
        write_insert_sql(load_sql, "items", vectors, args.insert_chunk)
        load_start = time.time()
        mysql_file(args.port, load_sql)
        log(f"loaded in {time.time() - load_start:.1f}s")

        for lib in libs:
            run = {"lib": lib}
            index_sql = (
                "use bench; create vector index idx_items on items(v) "
                f"with (distance=l2, type=hnsw, lib={lib}, m=16, "
                "ef_construction=200, ef_search=64);")
            first_query_sql = (
                "select id from items order by "
                f"l2_distance(v,'{vector_literal(queries[0])}') APPROXIMATE limit 1;")
            log(f"[{lib}] timing CREATE VECTOR INDEX + first query")
            start = time.time()
            proc = mysql(args.port, index_sql + first_query_sql, timeout=3600, check=False)
            build_seconds = time.time() - start
            if proc.returncode != 0:
                run["status"] = "failed"
                run["error"] = (proc.stderr or proc.stdout).strip()[-2000:]
                log(f"[{lib}] build FAILED: {run['error']}")
                results["runs"].append(run)
                mysql(args.port, "use bench; drop index idx_items on items;", check=False)
                continue
            run["status"] = "ok"
            run["build_seconds_with_first_query"] = round(build_seconds, 3)
            log(f"[{lib}] build+first query: {build_seconds:.2f}s")

            for k in ks:
                query_sql = args.out / f"query-{lib}-{k}.sql"
                write_query_sql(query_sql, k, queries)
                start = time.time()
                proc = mysql_file(args.port, query_sql, timeout=3600)
                elapsed = time.time() - start
                returned = parse_ids(proc.stdout, queries.shape[0], k)
                run[f"qps_top{k}"] = round(queries.shape[0] / elapsed, 2)
                run[f"avg_ms_top{k}"] = round(elapsed * 1000.0 / queries.shape[0], 3)
                run[f"recall_top{k}"] = round(recall(returned, truth, k), 4)
                log(f"[{lib}] top{k}: qps={run[f'qps_top{k}']} "
                    f"avg_ms={run[f'avg_ms_top{k}']} recall={run[f'recall_top{k}']}")

            mysql(args.port, "use bench; drop index idx_items on items;", check=False)
            results["runs"].append(run)

    results["status"] = "ok" if all(r.get("status") == "ok" for r in results["runs"]) else "partial"
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")

    lines = [
        "| lib | build+first_query (s) | Top-k | QPS | avg ms | Recall |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for run in results["runs"]:
        if run.get("status") != "ok":
            lines.append(f"| {run['lib']} | build failed | - | - | - | - |")
            continue
        for k in ks:
            lines.append(
                f"| {run['lib']} | {run['build_seconds_with_first_query']:.2f} | {k} | "
                f"{run[f'qps_top{k}']} | {run[f'avg_ms_top{k}']} | {run[f'recall_top{k}']} |")
    (args.out / "results.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))

    if not args.keep:
        shutil.rmtree(run_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
