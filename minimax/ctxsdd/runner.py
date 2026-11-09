"""测试执行与运行日志。"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from dataclasses import dataclass

from ctxsdd.config import Config


@dataclass
class PytestResult:
    name: str
    command: list[str]
    exit_code: int
    passed: int
    failed: int
    errors: int
    duration_s: float
    summary: str

    @property
    def ok(self) -> bool:
        return self.exit_code == 0 and self.failed == 0 and self.errors == 0

    def as_dict(self) -> dict:
        return {
            "name": self.name, "command": " ".join(self.command),
            "exit_code": self.exit_code, "passed": self.passed,
            "failed": self.failed, "errors": self.errors,
            "duration_s": round(self.duration_s, 2),
            "summary": self.summary, "ok": self.ok,
        }


def git_revision(root) -> str:
    try:
        r = subprocess.run(["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True)
        return r.stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def _parse_counts(output: str) -> tuple[int, int, int]:
    passed = failed = errors = 0
    for token, key in ((" passed", "passed"), (" failed", "failed"), (" error", "errors")):
        idx = output.find(token)
        if idx == -1:
            continue
        head = output[:idx]
        digits = ""
        for ch in reversed(head):
            if ch.isdigit() or ch == ",":
                digits = ch + digits
            else:
                break
        n = int(digits.replace(",", "") or 0)
        if key == "passed":
            passed = n
        elif key == "failed":
            failed = n
        else:
            errors = n
    return passed, failed, errors


def run_pytest(config: Config, name: str, targets: list[str]) -> PytestResult:
    command = [sys.executable, "-m", "pytest", "-q", *targets]
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=config.root, capture_output=True, text=True)
    duration = time.perf_counter() - started
    output = (completed.stdout or "") + (completed.stderr or "")
    passed, failed, errors = _parse_counts(output)
    summary_line = ""
    for line in reversed(output.splitlines()):
        if " passed" in line or " failed" in line or " error" in line or "no tests ran" in line:
            summary_line = line.strip()
            break
    result = PytestResult(name, command, completed.returncode, passed, failed, errors, duration, summary_line)
    log = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "name": name, "command": " ".join(command),
        "exit_code": completed.returncode, "passed": passed,
        "failed": failed, "errors": errors,
        "duration_s": round(duration, 2), "summary": summary_line,
        "git_revision": git_revision(config.root),
        "output_tail": "\n".join(output.splitlines()[-40:]),
    }
    with config.record("test_run_log.jsonl").open("a", encoding="utf-8") as f:
        f.write(json.dumps(log, ensure_ascii=False) + "\n")
    return result
