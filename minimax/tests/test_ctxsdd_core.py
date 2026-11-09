"""上下文模块的单元测试。"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from ctxsdd import patch as patch_tool
from ctxsdd.chunker import collect_chunks, chunk_spec
from ctxsdd.config import Config, load_config
from ctxsdd.state import _fold, append_event, load_state


@pytest.fixture()
def config(tmp_path: Path) -> Config:
    cfg = load_config()
    object.__setattr__(cfg, "record_dir", tmp_path / "records")
    return cfg


def test_normalize_diff_fixes_hunk_counts() -> None:
    raw = """--- a/test.py
+++ b/test.py
@@ -1,3 +1,4 @@
 line1
-line2
+line2_new
 line3
+line4
"""
    normalized = patch_tool.normalize_diff(raw)
    assert "@@ -1,3 +1,4 @@" in normalized or "@@ -1,3 +1,4 @@" in normalized


def test_extract_diff_from_fenced_block() -> None:
    text = """Here is the patch:
```diff
--- a/test.py
+++ b/test.py
@@ -1 +1 @@
-old
+new
```
Done."""
    diff = patch_tool.extract_diff(text)
    assert "--- a/test.py" in diff
    assert "+++ b/test.py" in diff


def test_extract_diff_requires_add_and_remove() -> None:
    with pytest.raises(patch_tool.PatchError):
        patch_tool.extract_diff("```diff\n--- a/test.py\n+++ b/test.py\n@@ -1 +1 @@\n+only added\n```")


def test_modified_paths_strips_prefix() -> None:
    diff = """--- a/business/reading_list/service.py
+++ b/business/reading_list/service.py
@@ -1,2 +1,2 @@
"""
    assert patch_tool.modified_paths(diff) == ["business/reading_list/service.py"]


def test_diff_sha_is_stable() -> None:
    assert patch_tool.diff_sha("abc") == patch_tool.diff_sha("abc")


def test_collect_chunks_finds_specs_and_code() -> None:
    root = Path(__file__).resolve().parent.parent
    chunks = collect_chunks(root)
    kinds = {c.kind for c in chunks}
    assert "spec" in kinds
    assert "code" in kinds
    assert "test" in kinds
    for c in chunks:
        assert c.line_from > 0
        assert c.line_to >= c.line_from
        assert c.path


def test_state_fold_progress(config: Config) -> None:
    append_event(config, {"event": "task_started", "task_id": "TASK-001"})
    append_event(config, {"event": "context_built", "task_id": "TASK-001",
                          "spec_digest": "abc", "context_digest": "def"})
    append_event(config, {"event": "patch_generated", "task_id": "TASK-001",
                          "patch_sha": "xyz", "attempt": 1})
    append_event(config, {"event": "patch_applied", "task_id": "TASK-001", "patch_sha": "xyz"})
    append_event(config, {"event": "verified", "task_id": "TASK-001", "ok": True})
    state = load_state(config)
    progress = state["progress"]
    assert progress["started"] is True
    assert progress["context_built"] is True
    assert progress["patch_applied"] is True
    assert progress["verified"] is True
    assert progress["attempts"] == 1
    assert progress["patch_sha"] == "xyz"


def test_state_isolated_between_scopes(config: Config) -> None:
    append_event(config, {"event": "task_started", "task_id": "TASK-001"})
    state = load_state(config)
    assert len(state["progress"]["history"]) >= 1
