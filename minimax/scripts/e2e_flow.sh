#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PY="${MM_PYTHON:-python3}"

echo "=== 1. Baseline: target tests should FAIL ==="
$PY -m pytest -q business/tests/test_filter_feature.py 2>&1 || true

echo ""
echo "=== 2. Baseline: regression tests should PASS ==="
$PY -m pytest -q business/tests/test_regression_baseline.py business/tests/test_http_api.py

echo ""
echo "=== 3. Init (import specs/code/tests to OceanBase) ==="
$PY -m ctxsdd init

echo ""
echo "=== 4. Plan (retrieve context pack) ==="
$PY -m ctxsdd plan

echo ""
echo "=== 5. Ask (MiniMax generates patch, fallback if no key) ==="
$PY -m ctxsdd ask

echo ""
echo "=== 6. Apply (apply patch) ==="
$PY -m ctxsdd apply

echo ""
echo "=== 7. Verify (target + regression tests) ==="
$PY -m ctxsdd verify

echo ""
echo "=== 8. State show (new process reads task state) ==="
$PY -m ctxsdd state-show

echo ""
echo "=== E2E flow complete ==="
