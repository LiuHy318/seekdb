"""MiniMax 开发助手客户端（OpenAI 兼容端点）。"""

from __future__ import annotations

import hashlib
import json
import time
from dataclasses import dataclass, field
from typing import Any, Sequence

import httpx

from ctxsdd.config import Config


class MiniMaxError(RuntimeError):
    pass


@dataclass
class MiniMaxResult:
    text: str
    model: str
    http_status: int
    finish_reason: str
    usage: dict[str, Any] = field(default_factory=dict)
    latency_ms: int = 0


def _sha(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _log(config: Config, record: dict[str, Any]) -> None:
    path = config.record("minimax_dev_log.jsonl")
    with path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(record, ensure_ascii=False) + "\n")


def chat(config: Config, messages: Sequence[dict[str, str]], *,
         max_tokens: int | None = None, purpose: str = "chat") -> MiniMaxResult:
    if not config.minimax_api_key:
        raise MiniMaxError("MM_MINIMAX_API_KEY is not set")
    budget = max_tokens or config.minimax_max_tokens
    started = time.perf_counter()
    response = httpx.post(
        config.minimax_base_url,
        headers={
            "Authorization": f"Bearer {config.minimax_api_key}",
            "Content-Type": "application/json",
        },
        json={
            "model": config.minimax_model,
            "messages": list(messages),
            "max_tokens": budget,
            "temperature": 0.2,
            "stream": False,
        },
        timeout=config.minimax_timeout,
    )
    latency_ms = int((time.perf_counter() - started) * 1000)
    if response.status_code != 200:
        raise MiniMaxError(f"HTTP {response.status_code}: {response.text[:400]}")
    body = response.json()
    choice = (body.get("choices") or [{}])[0]
    message = choice.get("message") or {}
    text = (message.get("content") or "").strip()
    finish_reason = choice.get("finish_reason") or ""
    _log(config, {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "purpose": purpose, "model": body.get("model", ""),
        "http_status": response.status_code, "finish_reason": finish_reason,
        "usage": body.get("usage", {}), "latency_ms": latency_ms,
        "prompt_sha256": _sha(json.dumps(list(messages), ensure_ascii=False, sort_keys=True)),
        "response_chars": len(text), "response": text,
    })
    if not text:
        raise MiniMaxError(f"empty content (finish_reason={finish_reason})")
    return MiniMaxResult(
        text=text, model=body.get("model", config.minimax_model),
        http_status=response.status_code, finish_reason=finish_reason,
        usage=body.get("usage", {}), latency_ms=latency_ms,
    )


SYSTEM_PROMPT = """你是 MiniMax 开发助手，在 SDD 流程中执行局部代码修改。
硬性要求：
1. 只修改满足规格所必需的最小范围。
2. 输出必须是一个 unified diff，放在 ```diff 代码块内。
3. diff 上下文行必须与给出的文件原文逐字一致。
4. 不要输出解释、不要输出测试结果。"""


def build_prompt(pack: dict[str, Any], files: dict[str, str], failure: str | None) -> str:
    parts = [
        "## 任务",
        f"- task_id: {pack['task']['task_id']}",
        f"- 标题: {pack['task']['title']}",
        f"- 目标: {pack['task']['goal']}",
        "", "## 必须保持不变的既有行为",
        *[f"- {i}" for i in pack["task"].get("must_preserve", [])],
        "", "## 适用规格条目（来自 OceanBase 检索）",
    ]
    for item in pack["specs"]["items"]:
        parts.append(f"### {item['path']} L{item['line_from']}-{item['line_to']}")
        parts.append(item.get("content", ""))
    parts += ["", "## 需要修改的代码位置"]
    for item in pack["code"]["items"]:
        parts.append(f"- {item['path']} :: {item.get('symbol', '')}")
    parts += ["", "## 验证用测试"]
    for p in pack["tests"]["target"]:
        parts.append(f"- {p}")
    if failure:
        parts += ["", "## 上次失败输出", "```text", failure[-3000:], "```"]
    parts += ["", "## 文件原文"]
    for path, content in files.items():
        parts += [f"### FILE: {path}", "```python", content, "```"]
    parts += ["", "现在输出 unified diff。"]
    return "\n".join(parts)


def generate_patch(config: Config, prompt: str, *, attempt: int = 1) -> MiniMaxResult:
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": prompt},
    ]
    return chat(config, messages, purpose=f"generate_patch#attempt{attempt}")
