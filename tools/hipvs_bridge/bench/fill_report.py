#!/usr/bin/env python3
"""Fill the contest CPU/GPU build-performance report from measured results.

`run_build_bench.py` writes one `results.json` per (build, dataset) pair. This
script combines the three artifacts the contest report asks for -- the frozen
baseline, the candidate, and the CPU (`lib=vsag`) reference -- into the exact
table of section 5 of `docs/gpu-vector-index-hipvs-cuvs/contest-optimization.md`,
and checks the recall gates that make a build time count at all.

Example:

    python3 tools/hipvs_bridge/bench/fill_report.py \\
      --dataset SIFT250K \\
      --baseline /tmp/bench/baseline-sift250k/results.json \\
      --candidate /tmp/bench/candidate-sift250k/results.json \\
      --cpu /tmp/bench/candidate-sift250k/results.json \\
      --out /tmp/bench/report-sift250k.md

The script only reads files it is given: it never invents a number. A row is
printed as `not measured` when the corresponding run is missing or failed, so a
half-filled report is obvious instead of silently wrong.
"""

import argparse
import json
import pathlib
import sys


RECALL_DELTA_LIMIT = 0.005


def load_results(path):
    try:
        return json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SystemExit(f"cannot read {path}: {exc}")


def pick_run(results, lib, index=0):
    runs = [run for run in results.get("runs", []) if run.get("lib") == lib]
    if len(runs) <= index:
        return None
    run = runs[index]
    return run if run.get("status") == "ok" else None


def fmt(value, digits=4):
    if value is None:
        return "not measured"
    return f"{value:.{digits}f}"


def row(dataset, backend, run, baseline_seconds):
    if run is None:
        return f"| {dataset} | {backend} | not measured | - | - | - |"
    seconds = run["build_seconds_with_first_query"]
    speedup = f"{baseline_seconds / seconds:.2f}x" if baseline_seconds and seconds else "-"
    if baseline_seconds and abs(seconds - baseline_seconds) < 1e-9:
        speedup = "1.00x"
    return (f"| {dataset} | {backend} | {seconds:.2f} | {speedup} | "
            f"{fmt(run.get('recall_top10'))} | {fmt(run.get('recall_top100'))} |")


def recall_floor(dataset, default10, default100):
    if "SIFT" in dataset.upper():
        return 0.95, 0.95
    return default10, default100


def gate_lines(dataset, candidate, baseline):
    if candidate is None or baseline is None:
        return ["- recall gates: not evaluated (baseline or candidate run missing)"]
    out = []
    r10, r100 = candidate.get("recall_top10"), candidate.get("recall_top100")
    b10, b100 = baseline.get("recall_top10"), baseline.get("recall_top100")
    if r10 is not None and b10 is not None:
        delta = r10 - b10
        out.append(f"- Recall@10: absolute {r10:.4f}, delta vs baseline {delta:+.4f} "
                   f"({'pass' if delta >= -RECALL_DELTA_LIMIT else 'FAIL'})")
    if r100 is not None and b100 is not None:
        delta = r100 - b100
        out.append(f"- Recall@100: absolute {r100:.4f}, delta vs baseline {delta:+.4f} "
                   f"({'pass' if delta >= -RECALL_DELTA_LIMIT else 'FAIL'})")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dataset", required=True,
                        help="dataset label used in the report, e.g. SIFT250K")
    parser.add_argument("--baseline", required=True,
                        help="results.json of the frozen baseline, lib=cuvs")
    parser.add_argument("--candidate", required=True,
                        help="results.json of this repository, lib=cuvs")
    parser.add_argument("--cpu", default="",
                        help="optional results.json of the lib=vsag CPU reference")
    parser.add_argument("--min-recall10", type=float, default=0.90,
                        help="absolute Recall@10 floor (SIFT is fixed at 0.95)")
    parser.add_argument("--min-recall100", type=float, default=0.85,
                        help="absolute Recall@100 floor (SIFT is fixed at 0.95)")
    parser.add_argument("--index", type=int, default=0,
                        help="which run of each lib to use when a file holds several")
    parser.add_argument("--out", default="",
                        help="write the markdown to this path instead of stdout")
    args = parser.parse_args()

    baseline_run = pick_run(load_results(args.baseline), "cuvs", args.index)
    candidate_run = pick_run(load_results(args.candidate), "cuvs", args.index)
    cpu_run = pick_run(load_results(args.cpu), "vsag", args.index) if args.cpu else None
    if baseline_run is None:
        raise SystemExit(f"no successful lib=cuvs run in {args.baseline}")
    baseline_seconds = baseline_run["build_seconds_with_first_query"]

    lines = [
        "| 数据集 | 后端 | 构建耗时（含第一条查询, s） | 加速比 | Recall@10 | Recall@100 |",
        "| --- | --- | ---: | ---: | ---: | ---: |",
        row(args.dataset, "冻结基线 `lib=cuvs`", baseline_run, baseline_seconds),
        row(args.dataset, "本仓库 `lib=cuvs`", candidate_run, baseline_seconds),
        row(args.dataset, "`lib=vsag`（CPU 参考）", cpu_run, baseline_seconds),
    ]

    floors = recall_floor(args.dataset, args.min_recall10, args.min_recall100)
    check = []
    if candidate_run is not None:
        for key, floor in (("recall_top10", floors[0]), ("recall_top100", floors[1])):
            value = candidate_run.get(key)
            verdict = "not measured"
            if value is not None:
                verdict = "pass" if value >= floor else "FAIL"
            check.append(f"- {key.replace('_', '@').upper()} floor {floor:.2f}: "
                         f"{fmt(value)} ({verdict})")
    check.extend(gate_lines(args.dataset, candidate_run, baseline_run))
    if candidate_run is not None and baseline_run is not None:
        ratio = candidate_run["build_seconds_with_first_query"] / baseline_seconds
        check.append(f"- build speedup vs baseline: {baseline_seconds / candidate_run['build_seconds_with_first_query']:.2f}x")

    report = "\n".join(lines) + "\n\n" + "\n".join(check) + "\n"
    if args.out:
        pathlib.Path(args.out).write_text(report, encoding="utf-8")
        print(f"wrote {args.out}")
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
