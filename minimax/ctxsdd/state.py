"""任务状态层（PowerContext 优先，SQLite JSON 兜底）。

如果 powercontext 包不可用，自动回落到 JSON 文件 journal，
保证「新进程读取并复用任务状态」的语义在任何环境都成立。
"""

from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path
from typing import Any

from ctxsdd.config import Config

EVENT_TASK_STARTED = "task_started"
EVENT_CONTEXT_BUILT = "context_built"
EVENT_PATCH_GENERATED = "patch_generated"
EVENT_PATCH_APPLIED = "patch_applied"
EVENT_TESTS_RUN = "tests_run"
EVENT_VERIFIED = "verified"
EVENT_SPEC_REIMPORTED = "spec_reimported"


def powercontext_version() -> str:
    try:
        from importlib.metadata import version
        return version("powercontext")
    except Exception:
        return "unavailable(json-fallback)"


def _journal_path(config: Config) -> Path:
    return config.record("state_journal.json")


def _append_json(config: Config, payload: dict[str, Any]) -> dict[str, Any]:
    """JSON 兜底：追加事件到 records/state_journal.json。"""
    path = _journal_path(config)
    path.parent.mkdir(parents=True, exist_ok=True)
    events: list[dict] = []
    if path.is_file():
        try:
            events = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            events = []
    body = dict(payload)
    body.setdefault("at", time.strftime("%Y-%m-%dT%H:%M:%S%z"))
    seq = len(events) + 1
    body["seq"] = seq
    body["source_id"] = f"{config.task_id}/evt-{seq:03d}"
    events.append(body)
    path.write_text(json.dumps(events, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {"seq": seq, "source_id": body["source_id"], "payload": body}


def _load_json(config: Config) -> dict[str, Any]:
    path = _journal_path(config)
    events: list[dict] = []
    if path.is_file():
        try:
            events = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            events = []
    return {"scope_id": config.pc_scope_id, "events": events, "progress": _fold(events)}


def _append_pc(config: Config, payload: dict[str, Any]) -> dict[str, Any]:
    """优先使用 PowerContext SDK。"""
    from powercontext.builtin.persistence.sqlite import SQLiteConfig
    from powercontext.builtin.runtime import BuiltinConfig, open_builtin_contexts
    from powercontext.builtin.sources.content import ContentCapture

    async def _run():
        body = dict(payload)
        body.setdefault("at", time.strftime("%Y-%m-%dT%H:%M:%S%z"))
        cfg = BuiltinConfig(database=SQLiteConfig(url=config.pc_database_url))
        async with open_builtin_contexts(cfg) as contexts:
            context = await contexts.get(config.pc_scope_id)
            entries = await context.sources.journal.entries()
            seq = len(entries) + 1
            body["seq"] = seq
            source_id = f"{config.task_id}/evt-{seq:03d}"
            await context.sources.capture(ContentCapture(
                source_id=source_id,
                content=json.dumps(body, ensure_ascii=False),
                metadata={"event": str(body.get("event", "")), "task_id": config.task_id},
            ))
        return {"seq": seq, "source_id": source_id, "payload": body}

    return asyncio.run(_run())


def _load_pc(config: Config) -> dict[str, Any]:
    from powercontext.builtin.persistence.sqlite import SQLiteConfig
    from powercontext.builtin.runtime import BuiltinConfig, open_builtin_contexts

    async def _run():
        cfg = BuiltinConfig(database=SQLiteConfig(url=config.pc_database_url))
        async with open_builtin_contexts(cfg) as contexts:
            context = await contexts.get(config.pc_scope_id)
            entries = await context.sources.journal.entries()
            events: list[dict] = []
            for entry in entries:
                try:
                    payload = json.loads(entry.source.content)
                except (json.JSONDecodeError, AttributeError):
                    payload = {"event": "unparsed", "raw": str(entry.source.content)[:200]}
                events.append(payload)
        return {"scope_id": config.pc_scope_id, "events": events, "progress": _fold(events)}

    return asyncio.run(_run())


def _has_powercontext() -> bool:
    try:
        import powercontext  # noqa: F401
        return True
    except ImportError:
        return False


def _fold(events: list[dict]) -> dict[str, Any]:
    progress: dict[str, Any] = {
        "task_id": None, "started": False, "context_built": False,
        "patch_generated": False, "patch_applied": False, "verified": False,
        "attempts": 0, "spec_digest": None, "context_digest": None,
        "patch_sha": None, "last_tests": None, "history": [],
    }
    for event in events:
        name = event.get("event")
        if event.get("task_id"):
            progress["task_id"] = event["task_id"]
        progress["history"].append({
            "seq": event.get("seq"), "event": name,
            "at": event.get("at"), "status": event.get("status"),
        })
        if name == EVENT_TASK_STARTED:
            progress["started"] = True
        elif name == EVENT_CONTEXT_BUILT:
            progress["context_built"] = True
            progress["spec_digest"] = event.get("spec_digest")
            progress["context_digest"] = event.get("context_digest")
        elif name == EVENT_PATCH_GENERATED:
            progress["patch_generated"] = True
            progress["patch_sha"] = event.get("patch_sha")
            progress["attempts"] = int(event.get("attempt", progress["attempts"] + 1))
        elif name == EVENT_PATCH_APPLIED:
            progress["patch_applied"] = True
            progress["patch_sha"] = event.get("patch_sha") or progress["patch_sha"]
        elif name == EVENT_TESTS_RUN:
            progress["last_tests"] = {
                "target_ok": event.get("target_ok"),
                "regression_ok": event.get("regression_ok"),
            }
        elif name == EVENT_VERIFIED:
            progress["verified"] = bool(event.get("ok"))
        elif name == EVENT_SPEC_REIMPORTED:
            progress["spec_digest"] = event.get("spec_digest_after", progress["spec_digest"])
    return progress


def append_event(config: Config, payload: dict[str, Any]) -> dict[str, Any]:
    if _has_powercontext():
        try:
            return _append_pc(config, payload)
        except Exception:
            pass
    return _append_json(config, payload)


def load_state(config: Config) -> dict[str, Any]:
    if _has_powercontext():
        try:
            return _load_pc(config)
        except Exception:
            pass
    return _load_json(config)
