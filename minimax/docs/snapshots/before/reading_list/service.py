"""列表查询服务：排序、分页（筛选待实现）。"""

from __future__ import annotations

from collections.abc import Iterable

from reading_list.models import Page, ReadingItem, Status

DEFAULT_PAGE_SIZE = 10
MAX_PAGE_SIZE = 100
DEFAULT_SORT = "added_at_desc"
SORT_MODES = (DEFAULT_SORT, "added_at_asc", "title_asc")


def _validate_page(page: int, page_size: int, sort: str) -> None:
    if not isinstance(page, int) or isinstance(page, bool) or page < 1:
        raise ValueError("page must be an integer >= 1")
    if not isinstance(page_size, int) or isinstance(page_size, bool):
        raise ValueError("page_size must be an integer")
    if page_size < 1 or page_size > MAX_PAGE_SIZE:
        raise ValueError(f"page_size must be within 1..{MAX_PAGE_SIZE}")
    if sort not in SORT_MODES:
        raise ValueError(f"sort must be one of {SORT_MODES}")


def _sort_key(sort: str):
    if sort == "added_at_asc":
        return lambda item: (item.added_at, item.id)
    if sort == "title_asc":
        return lambda item: (item.title, item.id)
    return lambda item: (-item.added_at.timestamp(), item.id)


class ReadingListService:
    """只读的阅读清单查询服务。"""

    def __init__(self, items: Iterable[ReadingItem]) -> None:
        self._items: tuple[ReadingItem, ...] = tuple(sorted(items, key=lambda item: item.id))

    @property
    def items(self) -> tuple[ReadingItem, ...]:
        return self._items

    def get_item(self, item_id: str) -> ReadingItem | None:
        for item in self._items:
            if item.id == item_id:
                return item
        return None

    def list_items(
        self,
        *,
        status: Status | str | None = None,
        min_rating: int | None = None,
        page: int = 1,
        page_size: int = DEFAULT_PAGE_SIZE,
        sort: str = DEFAULT_SORT,
    ) -> Page:
        _validate_page(page, page_size, sort)
        # TODO(SPEC-001): status / min_rating 过滤尚未实现，参数暂时被忽略。
        selected: list[ReadingItem] = list(self._items)
        ordered = sorted(selected, key=_sort_key(sort))
        total = len(ordered)
        total_pages = (total + page_size - 1) // page_size
        start = (page - 1) * page_size
        window = ordered[start:start + page_size]
        return Page(
            items=window, page=page, page_size=page_size,
            total=total, total_pages=total_pages,
        )


def list_all_items(
    items: Iterable[ReadingItem],
    *,
    status: Status | str | None = None,
    min_rating: int | None = None,
) -> list[ReadingItem]:
    service = ReadingListService(items)
    page = service.list_items(status=status, min_rating=min_rating, page=1, page_size=MAX_PAGE_SIZE)
    return list(page.items)
