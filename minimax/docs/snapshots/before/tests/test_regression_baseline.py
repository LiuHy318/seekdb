"""回归测试：SPEC-002（排序/分页）与 SPEC-003（错误与单条读取）。"""

from __future__ import annotations

import pytest

from reading_list.data import seed_items
from reading_list.models import Status
from reading_list.service import MAX_PAGE_SIZE, ReadingListService


@pytest.fixture()
def service() -> ReadingListService:
    return ReadingListService(seed_items())


def test_default_sort_is_added_at_desc(service: ReadingListService) -> None:
    page = service.list_items(page_size=12)
    assert [item.id for item in page.items] == [f"r{n:02d}" for n in range(12, 0, -1)]


def test_added_at_asc_sort(service: ReadingListService) -> None:
    page = service.list_items(page_size=12, sort="added_at_asc")
    assert [item.id for item in page.items] == [f"r{n:02d}" for n in range(1, 13)]


def test_title_asc_sort_is_stable_on_ties(service: ReadingListService) -> None:
    page = service.list_items(page_size=12, sort="title_asc")
    titles = [item.title for item in page.items]
    assert titles == sorted(titles)
    fowler = [item.id for item in page.items if item.author == "Martin Fowler"]
    assert fowler == ["r09", "r05"]


def test_pagination_first_page(service: ReadingListService) -> None:
    page = service.list_items(page=1, page_size=5)
    assert page.page == 1
    assert page.page_size == 5
    assert page.total == 12
    assert page.total_pages == 3
    assert [item.id for item in page.items] == ["r12", "r11", "r10", "r09", "r08"]


def test_pagination_last_page_is_partial(service: ReadingListService) -> None:
    page = service.list_items(page=3, page_size=5)
    assert [item.id for item in page.items] == ["r02", "r01"]
    assert page.total == 12
    assert page.total_pages == 3


def test_pagination_out_of_range_returns_empty_but_keeps_total(service: ReadingListService) -> None:
    page = service.list_items(page=4, page_size=5)
    assert page.items == []
    assert page.total == 12
    assert page.total_pages == 3


def test_total_pages_is_zero_for_empty_collection() -> None:
    page = ReadingListService([]).list_items()
    assert page.total == 0
    assert page.total_pages == 0
    assert page.items == []


@pytest.mark.parametrize("page", [0, -1])
def test_invalid_page_rejected(service: ReadingListService, page: int) -> None:
    with pytest.raises(ValueError):
        service.list_items(page=page)


@pytest.mark.parametrize("page_size", [0, -3, MAX_PAGE_SIZE + 1])
def test_invalid_page_size_rejected(service: ReadingListService, page_size: int) -> None:
    with pytest.raises(ValueError):
        service.list_items(page_size=page_size)


def test_unknown_sort_rejected(service: ReadingListService) -> None:
    with pytest.raises(ValueError):
        service.list_items(sort="title_desc")


def test_get_item_found_and_missing(service: ReadingListService) -> None:
    item = service.get_item("r03")
    assert item is not None
    assert item.status is Status.READING
    assert service.get_item("nope") is None


def test_status_enum_values_are_stable(service: ReadingListService) -> None:
    assert {item.status for item in service.items} == {Status.TO_READ, Status.READING, Status.FINISHED}
    assert {status.value for status in Status} == {"to_read", "reading", "finished"}
