"""补丁抽取与应用（内置 unified-diff 引擎，纯标准库）。"""

from __future__ import annotations

import hashlib
import re
import shutil
import time
from dataclasses import dataclass
from pathlib import Path

DIFF_FENCE = re.compile(r"```(?:diff|patch)?\s*\n(.*?)```", re.DOTALL)
HUNK_HEADER = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$")


class PatchError(RuntimeError):
    pass


@dataclass
class ApplyResult:
    ok: bool
    method: str
    returncode: int
    stdout: str
    stderr: str


def _count_hunk(lines: list[str]) -> tuple[int, int]:
    old = new = 0
    for line in lines:
        if line.startswith("@@ ") or line.startswith("diff --git "):
            break
        if line.startswith("\\"):
            continue
        if line.startswith("+"):
            new += 1
        elif line.startswith("-"):
            old += 1
        else:
            old += 1
            new += 1
    return old, new


def normalize_diff(diff: str) -> str:
    text = diff.replace("\r\n", "\n").replace("\r", "\n")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
    out: list[str] = []
    index = 0
    while index < len(lines):
        match = HUNK_HEADER.match(lines[index])
        if not match:
            out.append(lines[index])
            index += 1
            continue
        start = index + 1
        end = start
        while end < len(lines) and not HUNK_HEADER.match(lines[end]) and not lines[end].startswith("diff --git "):
            end += 1
        body = lines[start:end]
        for offset, bl in enumerate(body):
            if bl == "":
                body[offset] = " "
            elif not bl.startswith(("+", "-", " ", "\\")):
                body[offset] = " " + bl
        old, new = _count_hunk(body)
        tail = match.group(5) or ""
        out.append(f"@@ -{match.group(1)},{old} +{match.group(3)},{new} @@{tail}")
        out.extend(body)
        index = end
    return "\n".join(out).strip("\n") + "\n"


def extract_diff(text: str) -> str:
    candidate = ""
    for m in DIFF_FENCE.finditer(text):
        block = m.group(1)
        if "--- " in block and "+++ " in block:
            candidate = block
            break
    if not candidate:
        for i, line in enumerate(text.splitlines()):
            if line.startswith("--- ") or line.startswith("diff --git"):
                candidate = "\n".join(text.splitlines()[i:])
                break
    if not candidate:
        raise PatchError("no unified diff found")
    candidate = normalize_diff(candidate)
    removed = sum(1 for l in candidate.splitlines() if l.startswith("-") and not l.startswith("--- "))
    added = sum(1 for l in candidate.splitlines() if l.startswith("+") and not l.startswith("+++ "))
    if removed == 0 or added == 0:
        raise PatchError(f"diff has no added/removed lines (added={added}, removed={removed})")
    return candidate


def diff_sha(diff: str) -> str:
    return hashlib.sha256(diff.encode("utf-8")).hexdigest()


def modified_paths(diff: str) -> list[str]:
    paths: list[str] = []
    for line in diff.splitlines():
        for prefix in ("--- ", "+++ "):
            if line.startswith(prefix):
                raw = line[len(prefix):].strip().split("\t")[0]
                if raw in ("/dev/null", ""):
                    continue
                if raw.startswith(("a/", "b/")):
                    raw = raw[2:]
                if raw not in paths:
                    paths.append(raw)
    return paths


def _split_file_sections(diff: str) -> list[tuple[str, list[str]]]:
    lines = diff.split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
    sections: list[tuple[str, list[str]]] = []
    index = 0
    while index < len(lines):
        current_target = None
        if lines[index].startswith("--- "):
            raw = lines[index][4:].strip().split("\t")[0]
            if raw.startswith(("a/", "b/")):
                raw = raw[2:]
            current_target = raw
            index += 1
            if index < len(lines) and lines[index].startswith("+++ "):
                index += 1
        elif lines[index].startswith("diff --git "):
            index += 1
            continue
        else:
            index += 1
            continue
        body: list[str] = []
        while index < len(lines) and not lines[index].startswith("diff --git ") and not lines[index].startswith("--- "):
            body.append(lines[index])
            index += 1
        if current_target:
            sections.append((current_target, body))
    return sections


def _apply_hunks(path: Path, body: list[str]) -> tuple[bool, str]:
    if not path.is_file():
        return False, f"file not found: {path}"
    original = path.read_text(encoding="utf-8").split("\n")
    if original and original[-1] == "":
        original = original[:-1]
    result = list(original)
    offset = 0
    index = 0
    applied = 0
    while index < len(body):
        m = HUNK_HEADER.match(body[index])
        if not m:
            index += 1
            continue
        old_start = int(m.group(1))
        old_count = int(m.group(2) or 1)
        new_count = int(m.group(4) or 1)
        index += 1
        old_block: list[str] = []
        new_block: list[str] = []
        while index < len(body) and not HUNK_HEADER.match(body[index]):
            if len(old_block) >= old_count and len(new_block) >= new_count:
                break
            line = body[index]
            if line.startswith("\\"):
                index += 1
                continue
            if line.startswith("+"):
                new_block.append(line[1:])
            elif line.startswith("-"):
                old_block.append(line[1:])
            else:
                content = line[1:] if line.startswith(" ") else line
                old_block.append(content)
                new_block.append(content)
            index += 1
        anchor = max(0, old_start - 1 + offset)
        position = _locate(result, old_block, anchor)
        if position is None:
            return False, f"hunk context not found in {path.name}"
        result[position:position + len(old_block)] = new_block
        offset += len(new_block) - len(old_block)
        applied += 1
    if applied == 0:
        return False, f"no applicable hunk for {path.name}"
    path.write_text("\n".join(result) + "\n", encoding="utf-8")
    return True, f"{applied} hunk(s) applied"


def _locate(lines: list[str], block: list[str], anchor: int) -> int | None:
    if not block:
        return min(anchor, len(lines))
    limit = min(len(lines) - len(block), anchor + 20)
    for start in range(max(0, anchor - 20), max(0, limit) + 1):
        if lines[start:start + len(block)] == block:
            return start
    for start in range(0, len(lines) - len(block) + 1):
        if lines[start:start + len(block)] == block:
            return start
    return None


def apply_patch(root: Path, diff: str) -> ApplyResult:
    sections = _split_file_sections(diff)
    if not sections:
        return ApplyResult(False, "builtin", 1, "", "no file section")
    messages: list[str] = []
    for rel, body in sections:
        ok, msg = _apply_hunks(Path(root) / rel, body)
        messages.append(f"{rel}: {msg}")
        if not ok:
            return ApplyResult(False, "builtin", 1, "\n".join(messages), msg)
    return ApplyResult(True, "builtin", 0, "\n".join(messages), "")


def snapshot(root: Path, source: Path, destination: Path) -> dict[str, str]:
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns("__pycache__", ".pytest_cache", "*.pyc"))
    digests: dict[str, str] = {}
    for path in sorted(destination.rglob("*")):
        if path.is_file():
            rel = path.relative_to(destination).as_posix()
            digests[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
    return digests


def write_unified_diff(root: Path, before: Path, after: Path, output: Path) -> str:
    import difflib
    chunks: list[str] = []
    names = sorted(
        {p.relative_to(before).as_posix() for p in before.rglob("*") if p.is_file()} |
        {p.relative_to(after).as_posix() for p in after.rglob("*") if p.is_file()}
    )
    for name in names:
        left = before / name
        right = after / name
        if left.is_file() and right.is_file() and left.read_bytes() == right.read_bytes():
            continue
        old_lines = left.read_text(encoding="utf-8").splitlines(keepends=True) if left.is_file() else []
        new_lines = right.read_text(encoding="utf-8").splitlines(keepends=True) if right.is_file() else []
        chunks.extend(difflib.unified_diff(
            old_lines, new_lines,
            fromfile=f"a/business/{name}", tofile=f"b/business/{name}", n=3,
        ))
    text = "".join(chunks)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    return text


def stamp() -> str:
    return time.strftime("%Y%m%dT%H%M%S")
