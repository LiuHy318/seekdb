"""回归测试：HTTP 入口（SPEC-003）。"""

from __future__ import annotations

import json
import threading
import urllib.error
import urllib.request

import pytest

from reading_list import http_api


@pytest.fixture(scope="module")
def base_url():
    server = http_api.serve(port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    host, port = server.server_address[:2]
    try:
        yield f"http://{host}:{port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def _get(url: str) -> tuple[int, dict]:
    try:
        with urllib.request.urlopen(url, timeout=5) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read().decode("utf-8"))


def test_healthz(base_url: str) -> None:
    status, body = _get(f"{base_url}/healthz")
    assert status == 200
    assert body == {"status": "ok"}


def test_items_default_page(base_url: str) -> None:
    status, body = _get(f"{base_url}/items")
    assert status == 200
    assert body["total"] == 12
    assert body["page_size"] == 10
    assert len(body["items"]) == 10
    assert body["items"][0]["id"] == "r12"


def test_items_second_page(base_url: str) -> None:
    status, body = _get(f"{base_url}/items?page=2")
    assert status == 200
    assert [item["id"] for item in body["items"]] == ["r02", "r01"]


def test_items_invalid_page_returns_400(base_url: str) -> None:
    status, body = _get(f"{base_url}/items?page=0")
    assert status == 400
    assert "error" in body


def test_unknown_path_returns_404(base_url: str) -> None:
    status, body = _get(f"{base_url}/missing")
    assert status == 404
    assert body == {"error": "not found"}
