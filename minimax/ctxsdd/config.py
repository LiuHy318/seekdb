"""运行配置：全部来自环境变量，密钥不落盘。"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_dotenv(path: Path) -> None:
    """极简 .env 解析：KEY=VALUE，已存在的环境变量优先。"""
    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def _env(name: str, default: str) -> str:
    value = os.environ.get(name)
    return default if value is None or value == "" else value


def _env_int(name: str, default: int) -> int:
    try:
        return int(_env(name, str(default)))
    except ValueError:
        return default


@dataclass(frozen=True)
class Config:
    root: Path = ROOT
    ob_host: str = "127.0.0.1"
    ob_port: int = 2881
    ob_user: str = "root@sys"
    ob_password: str = ""
    ob_database: str = "mm_sdd"
    minimax_base_url: str = "https://api.minimax.chat/v1/chat/completions"
    minimax_api_key: str = ""
    minimax_model: str = "MiniMax-M3.1-Flash"
    minimax_max_tokens: int = 8192
    minimax_timeout: int = 300
    pc_scope_id: str = "mm-sdd-task-001"
    task_id: str = "TASK-001"
    record_dir: Path = field(default=ROOT / "records")

    @property
    def specs_dir(self) -> Path:
        return self.root / "specs"

    @property
    def business_dir(self) -> Path:
        return self.root / "business"

    @property
    def tasks_dir(self) -> Path:
        return self.root / "tasks"

    @property
    def snapshots_dir(self) -> Path:
        return self.root / "docs" / "snapshots"

    @property
    def pc_database_url(self) -> str:
        path = self.record_dir / "powercontext.db"
        return f"sqlite+aiosqlite:///{path}"

    def task_path(self, task_id: str | None = None) -> Path:
        return self.tasks_dir / f"{task_id or self.task_id}.json"

    def record(self, name: str) -> Path:
        self.record_dir.mkdir(parents=True, exist_ok=True)
        return self.record_dir / name


def load_config() -> Config:
    load_dotenv(ROOT / ".env")
    return Config(
        ob_host=_env("MM_OB_HOST", "127.0.0.1"),
        ob_port=_env_int("MM_OB_PORT", 2881),
        ob_user=_env("MM_OB_USER", "root@sys"),
        ob_password=_env("MM_OB_PASSWORD", ""),
        ob_database=_env("MM_OB_DATABASE", "mm_sdd"),
        minimax_base_url=_env("MM_MINIMAX_BASE_URL", "https://api.minimax.chat/v1/chat/completions"),
        minimax_api_key=_env("MM_MINIMAX_API_KEY", ""),
        minimax_model=_env("MM_MINIMAX_MODEL", "MiniMax-M3.1-Flash"),
        minimax_max_tokens=_env_int("MM_MINIMAX_MAX_TOKENS", 8192),
        minimax_timeout=_env_int("MM_MINIMAX_TIMEOUT", 300),
        pc_scope_id=_env("MM_PC_SCOPE", "mm-sdd-task-001"),
        task_id=_env("MM_TASK_ID", "TASK-001"),
    )
