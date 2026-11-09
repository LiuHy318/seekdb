"""领域模型：阅读条目与分页结果。"""

from __future__ import annotations

from datetime import datetime
from enum import Enum

from pydantic import BaseModel, Field


class Status(str, Enum):
    TO_READ = "to_read"
    READING = "reading"
    FINISHED = "finished"


class ReadingItem(BaseModel):
    id: str
    title: str
    author: str
    status: Status
    rating: int | None = Field(default=None, ge=1, le=5)
    added_at: datetime
    tags: list[str] = Field(default_factory=list)


class Page(BaseModel):
    items: list[ReadingItem]
    page: int
    page_size: int
    total: int
    total_pages: int
