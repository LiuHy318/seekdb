#!/usr/bin/env bash
# Sweep the CAGRA build knobs on one AMD GPU and print a comparison table.
#
# Each line of the --grid file is "graph_degree intermediate_degree niter algo",
# for example:
#
#   128 256 100 nn_descent    # frozen-baseline quality point
#   128 256  60 nn_descent
#   128 192  40 nn_descent
#    96 192  40 nn_descent
#   128 256  20 nn_descent
#   auto auto auto auto
#
# The baseline quality point must stay in the sweep: the contest gates recall
# against the frozen baseline (SIFT >= 0.995 Top-10, Hidden >= baseline-0.005),
# so a faster point only counts when its recall does not move.

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OBSERVER=""
BRIDGE=""
BASE_DIR=""
OUT=""
GRID=""
ARGS=()

usage()
{
  cat <<'EOF'
Usage: sweep.sh --observer PATH --bridge PATH --base-dir PATH --out DIR
                --grid FILE [--n N] [--dim D] [--nq Q] [--lib cuvs|vsag|both]
EOF
}

while (($# > 0)); do
  case "$1" in
    --observer) OBSERVER=$2; shift 2 ;;
    --bridge) BRIDGE=$2; shift 2 ;;
    --base-dir) BASE_DIR=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --grid) GRID=$2; shift 2 ;;
    --n|--dim|--nq|--lib|--port|--fvecs) ARGS+=("$1" "$2"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -x "$OBSERVER" ]] || { echo "observer is not executable: $OBSERVER" >&2; exit 2; }
[[ -f "$BRIDGE" ]] || { echo "bridge does not exist: $BRIDGE" >&2; exit 2; }
[[ -f "$GRID" ]] || { echo "grid file does not exist: $GRID" >&2; exit 2; }
mkdir -p "$OUT"

printf '| build_algo | graph_degree | intermediate_degree | niter | build+first_query (s) | Top-10 recall | Top-100 recall |\n'
printf '| --- | ---: | ---: | ---: | ---: | ---: | ---: |\n'

index=0
while read -r gd igd niter algo; do
  [[ -z "${gd:-}" || "$gd" == \#* ]] && continue
  index=$((index + 1))
  run_dir="$OUT/run-$index"
  rm -rf "$run_dir"
  echo "[sweep] run $index: algo=$algo gd=$gd igd=$igd niter=$niter" >&2
  python3 "$SCRIPT_DIR/run_build_bench.py" \
    --observer "$OBSERVER" --bridge "$BRIDGE" \
    --base-dir "$BASE_DIR" --out "$run_dir" \
    "${ARGS[@]}" \
    --param "SEEKDB_CUVS_BUILD_ALGO=$algo" \
    --param "SEEKDB_CUVS_GRAPH_DEGREE=$gd" \
    --param "SEEKDB_CUVS_INTERMEDIATE_DEGREE=$igd" \
    --param "SEEKDB_CUVS_NITER=$niter" >/dev/null

  python3 - "$run_dir/results.json" "$algo" "$gd" "$igd" "$niter" <<'PY'
import json
import sys

path, algo, gd, igd, niter = sys.argv[1:]
data = json.load(open(path))
run = next((item for item in data["runs"] if item["lib"] == "cuvs"), None)
if run is None or run.get("status") != "ok":
    print(f"| {algo} | {gd} | {igd} | {niter} | build failed | - | - |")
else:
    print(f"| {algo} | {gd} | {igd} | {niter} | "
          f"{run['build_seconds_with_first_query']:.2f} | "
          f"{run.get('recall_top10', float('nan'))} | "
          f"{run.get('recall_top100', float('nan'))} |")
PY
done < "$GRID"

echo "[sweep] results under $OUT" >&2
