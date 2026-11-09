#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PY="${MM_PYTHON:-python3}"

echo "=== Before spec update ==="
$PY -m ctxsdd plan --task TASK-001 2>/dev/null | python3 -c "import sys,json; d=json.load(sys.stdin); print('spec_digest:', d['pack']['specs']['fingerprint']['digest'][:16])"

# Temporarily modify a spec
cp specs/SPEC-001-list-filter.md /tmp/spec_backup.md
echo "" >> specs/SPEC-001-list-filter.md
echo "## 6. Additional note (temporary for verification)" >> specs/SPEC-001-list-filter.md

echo ""
echo "=== After spec update ==="
$PY -m ctxsdd init 2>/dev/null
$PY -m ctxsdd plan --task TASK-001 2>/dev/null | python3 -c "import sys,json; d=json.load(sys.stdin); print('spec_digest:', d['pack']['specs']['fingerprint']['digest'][:16])"

# Restore spec
cp /tmp/spec_backup.md specs/SPEC-001-list-filter.md
$PY -m ctxsdd init 2>/dev/null

echo ""
echo "=== Spec update check done (digest should change between runs) ==="
