"""初始数据：12 条确定性阅读记录。"""

from __future__ import annotations

from datetime import datetime, timezone

from reading_list.models import ReadingItem, Status

_BASE = datetime(2026, 1, 1, 8, 0, 0, tzinfo=timezone.utc)

_RAW = [
    ("r01", "Clean Architecture", "Robert C. Martin", Status.FINISHED, 5, 0, ["design"]),
    ("r02", "Designing Data-Intensive Applications", "Martin Kleppmann", Status.FINISHED, 5, 1, ["distributed"]),
    ("r03", "Working Effectively with Legacy Code", "Michael Feathers", Status.READING, 4, 2, ["refactor"]),
    ("r04", "The Pragmatic Programmer", "Andrew Hunt", Status.FINISHED, 4, 3, ["craft"]),
    ("r05", "Refactoring", "Martin Fowler", Status.READING, None, 4, ["refactor"]),
    ("r06", "Domain-Driven Design", "Eric Evans", Status.TO_READ, None, 5, ["design"]),
    ("r07", "Accelerate", "Nicole Forsgren", Status.FINISHED, 3, 6, ["delivery"]),
    ("r08", "Site Reliability Engineering", "Betsy Beyer", Status.READING, 5, 7, ["ops"]),
    ("r09", "Patterns of Enterprise Application Architecture", "Martin Fowler", Status.TO_READ, None, 8, ["design"]),
    ("r10", "Continuous Delivery", "Jez Humble", Status.TO_READ, None, 9, ["delivery"]),
    ("r11", "A Philosophy of Software Design", "John Ousterhout", Status.FINISHED, 4, 10, ["design"]),
    ("r12", "Database Internals", "Alex Petrov", Status.READING, 3, 11, ["database"]),
]


def seed_items() -> list[ReadingItem]:
    items: list[ReadingItem] = []
    for item_id, title, author, status, rating, offset_days, tags in _RAW:
        items.append(ReadingItem(
            id=item_id, title=title, author=author, status=status,
            rating=rating, added_at=_BASE.replace(day=1 + offset_days), tags=list(tags),
        ))
    return items


def default_service():
    from reading_list.service import ReadingListService
    return ReadingListService(seed_items())
