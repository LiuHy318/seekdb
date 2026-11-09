#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# 回到基线：从 docs/snapshots/before 恢复 business/
rm -rf business/
cp -r docs/snapshots/before/ business/
find business -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null || true

echo "=== Reset to baseline done ==="
