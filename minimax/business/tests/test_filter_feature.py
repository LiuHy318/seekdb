"""目标功能测试：TASK-001（SPEC-001 按状态 + 最低评分筛选）。"""

from __future__ import annotations

import pytest

from reading_list.data import seed_items
from reading_list.models import Status
from reading_list.service import ReadingListService, list_all_items


@pytest.fixture()
def service() -> ReadingListService:
    return ReadingListService(seed_items())


def test_filter_by_status_finished(service: ReadingListService) -> None:
    page = service.list_items(status=Status.FINISHED, page_size=12)
    assert [item.id for item in page.items] == ["r11", "r07", "r04", "r02", "r01"]
    assert page.total == 5
    assert page.total_pages == 1
    assert {item.status for item in page.items} == {Status.FINISHED}


def test_filter_by_status_accepts_plain_string(service: ReadingListService) -> None:
    page = service.list_items(status="to_read", page_size=12)
    assert page.total == 3
    assert {item.status for item in page.items} == {Status.TO_READ}


def test_filter_by_min_rating_excludes_unrated(service: ReadingListService) -> None:
    page = service.list_items(min_rating=4, page_size=12)
    assert [item.id for item in page.items] == ["r11", "r08", "r04", "r03", "r02", "r01"]
    assert page.total == 6
    assert all(item.rating is not None and item.rating >= 4 for item in page.items)


def test_filter_status_and_min_rating_combine_with_and(service: ReadingListService) -> None:
    page = service.list_items(status=Status.FINISHED, min_rating=4, page_size=12)
    assert [item.id for item in page.items] == ["r11", "r04", "r02", "r01"]
    assert page.total == 4


def test_filter_result_can_be_empty(service: ReadingListService) -> None:
    page = service.list_items(status=Status.TO_READ, min_rating=5)
    assert page.items == []
    assert page.total == 0
    assert page.total_pages == 0


def test_filter_keeps_ordering_and_pagination(service: ReadingListService) -> None:
    first = service.list_items(status=Status.READING, page=1, page_size=2)
    second = service.list_items(status=Status.READING, page=2, page_size=2)
    assert [item.id for item in first.items] == ["r12", "r08"]
    assert [item.id for item in second.items] == ["r05", "r03"]
    assert first.total == 4
    assert first.total_pages == 2


def test_filter_respects_ascending_sort(service: ReadingListService) -> None:
    page = service.list_items(status=Status.READING, sort="added_at_asc", page_size=12)
    assert [item.id for item in page.items] == ["r03", "r05", "r08", "r12"]


@pytest.mark.parametrize("min_rating", [0, -1, 6, 99])
def test_min_rating_out_of_range_rejected(service: ReadingListService, min_rating: int) -> None:
    with pytest.raises(ValueError):
        service.list_items(min_rating=min_rating)


def test_invalid_status_value_rejected(service: ReadingListService) -> None:
    with pytest.raises(ValueError):
        service.list_items(status="paused")


def test_list_all_items_supports_filtering() -> None:
    items = list_all_items(seed_items(), status=Status.READING, min_rating=4)
    assert [item.id for item in items] == ["r08", "r03"]


def test_filter_does_not_mutate_service_state(service: ReadingListService) -> None:
    page = service.list_items(status=Status.FINISHED)
    assert page.total == 5
    assert len(page.items) == 5
    assert len(service.items) == 12
    assert service.list_items(page_size=12).total == 12
