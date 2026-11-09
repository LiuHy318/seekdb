"""上下文包：把 OceanBase 检索结果 + 进度组装成开发上下文。"""

from __future__ import annotations

import hashlib
import json
import time
from typing import Any, Sequence

from ctxsdd.config import Config
from ctxsdd.obstore import Hit, OceanBaseStore


def _digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def spec_fingerprint(store: OceanBaseStore) -> dict[str, Any]:
    rows = store.spec_rows()
    joined = "\n".join(f"{p}:{s}" for p, s in rows)
    return {
        "chunks": [{"path": p, "content_sha": s} for p, s in rows],
        "digest": _digest(joined),
        "count": len(rows),
    }


def _group(hits: Sequence[Hit], *, with_content: bool = False) -> list[dict[str, Any]]:
    return [h.locator(with_content=with_content) for h in hits]


def build_context_pack(
    config: Config,
    store: OceanBaseStore,
    task: dict[str, Any],
    *,
    progress: dict[str, Any],
) -> dict[str, Any]:
    terms = [str(t) for t in task.get("query_terms", [])]
    spec_hits = store.search(terms, kinds=["spec"], limit=6)
    code_hits = store.search(terms, kinds=["code"], limit=8)
    test_hits = store.search(terms, kinds=["test"], limit=6)

    code_items = _group(code_hits)
    test_items = _group(test_hits)

    covered_code = {i["path"] for i in code_items}
    for path in sorted(set(task.get("target_files", [])) - covered_code):
        code_items.append({"path": path, "kind": "code", "symbol": "", "source": "task-anchor"})
    covered_test = {i["path"] for i in test_items}
    test_anchors = set(task.get("target_tests", [])) | set(task.get("regression_tests", []))
    for path in sorted(test_anchors - covered_test):
        test_items.append({"path": path, "kind": "test", "symbol": "", "source": "task-anchor"})

    for item in code_items:
        item.setdefault("source", "retrieval")
    for item in test_items:
        item.setdefault("source", "retrieval")

    pack = {
        "schema": "ctxsdd/context-pack/v1",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "task": {
            "task_id": task.get("task_id", config.task_id),
            "title": task.get("title", ""),
            "goal": task.get("goal", ""),
            "must_preserve": task.get("must_preserve", []),
        },
        "retrieval": {
            "engine": "OceanBase/seekdb",
            "database": config.ob_database,
            "table": "mm_doc",
            "terms": terms,
        },
        "specs": {
            "spec_ids": task.get("spec_ids", []),
            "fingerprint": spec_fingerprint(store),
            "items": _group(spec_hits, with_content=True),
        },
        "code": {"items": code_items},
        "tests": {
            "target": task.get("target_tests", []),
            "regression": task.get("regression_tests", []),
            "items": test_items,
        },
        "progress": {
            "source": "PowerContext",
            "scope_id": config.pc_scope_id,
            "events": len(progress.get("history", [])),
            "context_built": progress.get("context_built", False),
            "patch_generated": progress.get("patch_generated", False),
            "patch_applied": progress.get("patch_applied", False),
            "verified": progress.get("verified", False),
            "attempts": progress.get("attempts", 0),
        },
    }
    pack["context_digest"] = _digest(json.dumps(pack, ensure_ascii=False, sort_keys=True))
    return pack


def write_pack(config: Config, pack: dict[str, Any], name: str = "context_pack.json") -> str:
    path = config.record(name)
    path.write_text(json.dumps(pack, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return str(path)
