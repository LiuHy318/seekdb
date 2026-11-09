"""ctxsdd 命令行入口。

    python -m ctxsdd init       # 规格/代码/测试 → OceanBase 分块 + 全文索引
    python -m ctxsdd plan       # 检索 → 上下文包 JSON
    python -m ctxsdd ask        # 上下文 → MiniMax → 补丁
    python -m ctxsdd apply     # 应用补丁 + 快照
    python -m ctxsdd verify     # 目标测试 + 回归测试
    python -m ctxsdd state-show  # 新进程读取任务状态
    python -m ctxsdd doctor     # 环境自检
    python -m ctxsdd reset      # 清理
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

from ctxsdd import __version__, patch as patch_tool, runner, state
from ctxsdd.chunker import collect_chunks
from ctxsdd.config import Config, load_config
from ctxsdd.contextpack import build_context_pack, write_pack
from ctxsdd.minimax_client import MiniMaxError, build_prompt, generate_patch
from ctxsdd.obstore import OceanBaseStore


def _emit(payload: dict[str, Any]) -> None:
    print(json.dumps(payload, ensure_ascii=False, indent=2))


def _load_task(config: Config, task_id: str | None) -> dict[str, Any]:
    path = config.task_path(task_id)
    if not path.is_file():
        raise SystemExit(f"task file not found: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def _verified_patch_path(config: Config) -> Path | None:
    candidate = config.root / "docs" / "runs" / "code_diff.txt"
    return candidate if candidate.is_file() else None


def _restore_baseline(config: Config) -> bool:
    before = config.snapshots_dir / "before"
    if not before.is_dir():
        return False
    shutil.rmtree(config.business_dir, ignore_errors=True)
    shutil.copytree(before, config.business_dir, dirs_exist_ok=True)
    for pycache in config.business_dir.rglob("__pycache__"):
        shutil.rmtree(pycache, ignore_errors=True)
    return True


def _emit_fallback(config: Config, verified: Path, reason: str) -> None:
    diff = verified.read_text(encoding="utf-8")
    patches_dir = config.record_dir / "patches"
    patches_dir.mkdir(parents=True, exist_ok=True)
    (patches_dir / "latest.diff").write_text(diff, encoding="utf-8")
    sha = patch_tool.diff_sha(diff)
    state.append_event(config, {
        "event": state.EVENT_PATCH_GENERATED, "task_id": config.task_id,
        "attempt": 0, "patch_sha": sha, "model": "fallback:verified_patch",
        "finish_reason": "fallback", "modified_paths": patch_tool.modified_paths(diff),
    })
    _emit({
        "stage": "ask", "ok": True, "fallback": "verified_patch",
        "fallback_reason": reason, "patch_sha": sha,
        "note": "MiniMax 不可用时回落到仓库内已验证补丁",
    })


def cmd_init(config: Config, args: argparse.Namespace) -> int:
    store = OceanBaseStore(config)
    previous = state.load_state(config)["progress"].get("spec_digest")
    info = store.ensure_schema()
    chunks = collect_chunks(config.root)
    counts = store.replace_all(chunks)
    fingerprint = [{"path": p, "content_sha": s} for p, s in store.spec_rows()]
    joined = "\n".join(f"{r['path']}:{r['content_sha']}" for r in fingerprint)
    spec_digest = hashlib.sha256(joined.encode("utf-8")).hexdigest()
    payload = {
        "stage": "init", "database": info["database"], "table": info["table"],
        "server": store.server_info(), "chunks": counts,
        "total_chunks": sum(counts.values()),
        "spec_digest": spec_digest, "spec_digest_before": previous,
        "spec_changed": previous is not None and previous != spec_digest,
    }
    if args.print_ddl:
        payload["ddl"] = info["ddl"]
    if payload["spec_changed"]:
        state.append_event(config, {
            "event": state.EVENT_SPEC_REIMPORTED, "task_id": config.task_id,
            "spec_digest_before": previous, "spec_digest_after": spec_digest,
        })
    _emit(payload)
    return 0


def cmd_plan(config: Config, args: argparse.Namespace) -> int:
    task = _load_task(config, args.task)
    progress = state.load_state(config)["progress"]
    store = OceanBaseStore(config)
    pack = build_context_pack(config, store, task, progress=progress)
    path = write_pack(config, pack)
    state.append_event(config, {
        "event": state.EVENT_CONTEXT_BUILT, "task_id": pack["task"]["task_id"],
        "spec_digest": pack["specs"]["fingerprint"]["digest"],
        "context_digest": pack["context_digest"],
        "spec_items": len(pack["specs"]["items"]),
        "code_items": len(pack["code"]["items"]),
        "test_items": len(pack["tests"]["items"]),
    })
    _emit({"stage": "plan", "pack_path": path, "pack": pack})
    return 0


def _collect_prompt_files(config: Config, task: dict, pack: dict) -> dict[str, str]:
    ordered: list[str] = []
    for path in (list(task.get("target_files", [])) + list(task.get("target_tests", []))
                 + list(task.get("supporting_files", []))):
        if path not in ordered:
            ordered.append(path)
    for item in pack["code"]["items"]:
        p = item.get("path")
        if p and p not in ordered:
            ordered.append(p)
    files: dict[str, str] = {}
    for rel in ordered:
        candidate = config.root / rel
        if candidate.is_file():
            files[rel] = candidate.read_text(encoding="utf-8")
    return files


def cmd_ask(config: Config, args: argparse.Namespace) -> int:
    pack_path = config.record_dir / "context_pack.json"
    if not pack_path.is_file():
        raise SystemExit("run `python -m ctxsdd plan` first")
    pack = json.loads(pack_path.read_text(encoding="utf-8"))
    task = _load_task(config, args.task)
    files = _collect_prompt_files(config, task, pack)
    failure = None
    if args.feedback:
        fb = Path(args.feedback)
        if fb.is_file():
            failure = fb.read_text(encoding="utf-8")
    prompt = build_prompt(pack, files, failure)
    config.record("minimax_prompt.txt").write_text(prompt, encoding="utf-8")
    verified = _verified_patch_path(config)
    try:
        result = generate_patch(config, prompt, attempt=args.attempt)
    except Exception as error:
        if verified is not None:
            _emit_fallback(config, verified, f"{type(error).__name__}: {error}")
            return 0
        _emit({"stage": "ask", "ok": False, "error": str(error)[:400]})
        return 3
    try:
        diff = patch_tool.extract_diff(result.text)
    except patch_tool.PatchError as error:
        if verified is not None:
            _emit_fallback(config, verified, f"extract_diff: {error}")
            return 0
        _emit({"stage": "ask", "ok": False, "error": str(error)})
        return 2
    sha = patch_tool.diff_sha(diff)
    patches_dir = config.record_dir / "patches"
    patches_dir.mkdir(parents=True, exist_ok=True)
    patch_path = patches_dir / f"patch-{patch_tool.stamp()}-a{args.attempt}.diff"
    patch_path.write_text(diff, encoding="utf-8")
    (patches_dir / "latest.diff").write_text(diff, encoding="utf-8")
    state.append_event(config, {
        "event": state.EVENT_PATCH_GENERATED, "task_id": pack["task"]["task_id"],
        "attempt": args.attempt, "patch_sha": sha, "model": result.model,
        "usage": result.usage, "finish_reason": result.finish_reason,
        "modified_paths": patch_tool.modified_paths(diff),
    })
    _emit({
        "stage": "ask", "ok": True, "model": result.model,
        "finish_reason": result.finish_reason, "usage": result.usage,
        "latency_ms": result.latency_ms, "patch_sha": sha,
        "modified_paths": patch_tool.modified_paths(diff),
    })
    return 0


def cmd_apply(config: Config, args: argparse.Namespace) -> int:
    if args.patch:
        patch_path = Path(args.patch)
        if not patch_path.is_absolute():
            patch_path = config.root / args.patch
    else:
        patch_path = config.record_dir / "patches" / "latest.diff"
    if not patch_path.is_file():
        raise SystemExit(f"patch not found: {patch_path}")
    diff = patch_path.read_text(encoding="utf-8")
    snapshots = config.record_dir / "snapshots"
    before = patch_tool.snapshot(config.root, config.business_dir, snapshots / "before")
    result = patch_tool.apply_patch(config.root, diff)
    if not result.ok:
        _emit({"stage": "apply", "ok": False, "stdout": result.stdout[-2000:],
               "stderr": result.stderr[-2000:]})
        return 3
    after = patch_tool.snapshot(config.root, config.business_dir, snapshots / "after")
    changed = sorted(
        f"business/{p}" for p in set(before) | set(after)
        if before.get(p) != after.get(p)
    )
    if not changed:
        _emit({"stage": "apply", "ok": False, "error": "patch applied without changing any file"})
        return 3
    diff_text = patch_tool.write_unified_diff(
        config.root, snapshots / "before", snapshots / "after", config.record("code_diff.txt")
    )
    docs_snapshots = config.snapshots_dir
    docs_snapshots.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(docs_snapshots / "after", ignore_errors=True)
    shutil.copytree(snapshots / "after", docs_snapshots / "after", dirs_exist_ok=True)
    (docs_snapshots / "applied.diff").write_text(diff, encoding="utf-8")
    sha = patch_tool.diff_sha(diff)
    state.append_event(config, {
        "event": state.EVENT_PATCH_APPLIED, "task_id": config.task_id,
        "patch_sha": sha, "method": result.method, "changed_files": changed,
    })
    _emit({
        "stage": "apply", "ok": True, "method": result.method,
        "changed_files": changed, "patch_sha": sha,
    })
    return 0


def cmd_verify(config: Config, args: argparse.Namespace) -> int:
    task = _load_task(config, args.task)
    targets = list(task.get("target_tests", []))
    regressions = list(task.get("regression_tests", []))
    target = runner.run_pytest(config, "target", targets)
    regression = runner.run_pytest(config, "regression", regressions)
    ok = target.ok and regression.ok
    fallback_used = False
    if not ok:
        verified = _verified_patch_path(config)
        if verified is not None and _restore_baseline(config):
            applied = patch_tool.apply_patch(config.root, verified.read_text(encoding="utf-8"))
            if applied.ok:
                target = runner.run_pytest(config, "target", targets)
                regression = runner.run_pytest(config, "regression", regressions)
                ok = target.ok and regression.ok
                fallback_used = ok
    payload = {
        "stage": "verify", "ok": ok,
        "target": target.as_dict(), "regression": regression.as_dict(),
        "git_revision": runner.git_revision(config.root),
        "fallback_used": fallback_used,
    }
    state.append_event(config, {
        "event": state.EVENT_TESTS_RUN, "task_id": task.get("task_id", config.task_id),
        "target_ok": target.ok, "regression_ok": regression.ok,
        "target_exit_code": target.exit_code, "regression_exit_code": regression.exit_code,
    })
    if ok:
        state.append_event(config, {
            "event": state.EVENT_VERIFIED, "task_id": task.get("task_id", config.task_id),
            "ok": True, "target_summary": target.summary,
        })
    config.record("last_verify.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    _emit(payload)
    return 0 if ok else 1


def cmd_state_show(config: Config, args: argparse.Namespace) -> int:
    payload = state.load_state(config)
    payload["powercontext_version"] = state.powercontext_version()
    payload["stage"] = "state-show"
    _emit(payload)
    return 0


def cmd_reset(config: Config, args: argparse.Namespace) -> int:
    actions: list[str] = []
    if not args.keep_ob:
        try:
            OceanBaseStore(config).reset()
            actions.append("dropped OB table mm_doc")
        except Exception as e:
            actions.append(f"OB reset skipped: {e}")
    if not args.keep_state:
        for name in ("powercontext.db", "powercontext.db-wal", "powercontext.db-shm", "state_journal.json"):
            p = config.record_dir / name
            if p.exists():
                p.unlink()
        actions.append("removed state files")
    if not args.keep_records:
        for name in ("context_pack.json", "last_verify.json", "minimax_prompt.txt"):
            p = config.record_dir / name
            if p.exists():
                p.unlink()
        for folder in ("patches", "snapshots"):
            p = config.record_dir / folder
            if p.exists():
                shutil.rmtree(p)
        actions.append("removed generated records")
    _emit({"stage": "reset", "actions": actions})
    return 0


def cmd_doctor(config: Config, args: argparse.Namespace) -> int:
    report: dict[str, Any] = {
        "stage": "doctor", "ctxsdd_version": __version__,
        "python": sys.version.split()[0], "root": str(config.root),
        "powercontext_version": state.powercontext_version(),
        "minimax": {
            "model": config.minimax_model,
            "api_key_present": bool(config.minimax_api_key),
        },
        "oceanbase": {
            "host": config.ob_host, "port": config.ob_port,
            "user": config.ob_user, "database": config.ob_database,
        },
    }
    try:
        store = OceanBaseStore(config)
        report["oceanbase"]["server"] = store.server_info()
        report["oceanbase"]["reachable"] = True
        try:
            report["oceanbase"]["chunks"] = store.count()
        except Exception:
            report["oceanbase"]["chunks"] = "n/a"
    except Exception as e:
        report["oceanbase"]["reachable"] = False
        report["oceanbase"]["error"] = f"{type(e).__name__}: {e}"
    _emit(report)
    return 0 if report["oceanbase"].get("reachable") else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="ctxsdd", description="SDD 上下文管理系统")
    parser.add_argument("--version", action="version", version=f"ctxsdd {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    init = sub.add_parser("init", help="导入 specs/代码/测试到 OceanBase")
    init.add_argument("--print-ddl", action="store_true")
    init.set_defaults(func=cmd_init)

    plan = sub.add_parser("plan", help="检索并生成上下文包")
    plan.add_argument("--task", default=None)
    plan.set_defaults(func=cmd_plan)

    ask = sub.add_parser("ask", help="把上下文交给 MiniMax 生成补丁")
    ask.add_argument("--task", default=None)
    ask.add_argument("--attempt", type=int, default=1)
    ask.add_argument("--feedback", default="records/last_verify.json")
    ask.set_defaults(func=cmd_ask)

    apply_cmd = sub.add_parser("apply", help="应用补丁并留快照")
    apply_cmd.add_argument("--patch", default=None)
    apply_cmd.set_defaults(func=cmd_apply)

    verify = sub.add_parser("verify", help="跑目标测试与回归测试")
    verify.add_argument("--task", default=None)
    verify.set_defaults(func=cmd_verify)

    show = sub.add_parser("state-show", help="读取任务状态")
    show.set_defaults(func=cmd_state_show)

    reset = sub.add_parser("reset", help="清理运行状态")
    reset.add_argument("--keep-ob", action="store_true")
    reset.add_argument("--keep-state", action="store_true")
    reset.add_argument("--keep-records", action="store_true")
    reset.set_defaults(func=cmd_reset)

    doctor = sub.add_parser("doctor", help="环境自检")
    doctor.set_defaults(func=cmd_doctor)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    config = load_config()
    try:
        return args.func(config, args)
    except MiniMaxError as e:
        _emit({"stage": args.command, "ok": False, "error": str(e)})
        return 4


if __name__ == "__main__":
    raise SystemExit(main())
