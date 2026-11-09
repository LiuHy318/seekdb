"""最小 HTTP 入口：GET /items 与 GET /healthz。"""

from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from reading_list.data import default_service
from reading_list.service import DEFAULT_PAGE_SIZE

_SERVICE = default_service()


def _int_param(params: dict[str, list[str]], name: str, default: int) -> int:
    raw = params.get(name, [None])[0]
    if raw is None or raw == "":
        return default
    try:
        return int(raw)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc


def handle_items(query: str) -> tuple[int, dict]:
    params = parse_qs(query, keep_blank_values=True)
    status = params.get("status", [None])[0] or None
    min_rating_raw = params.get("min_rating", [None])[0] or None
    min_rating = None if min_rating_raw is None else _int_param(params, "min_rating", 0)
    try:
        page = _SERVICE.list_items(
            status=status,
            min_rating=min_rating,
            page=_int_param(params, "page", 1),
            page_size=_int_param(params, "page_size", DEFAULT_PAGE_SIZE),
            sort=params.get("sort", ["added_at_desc"])[0] or "added_at_desc",
        )
    except ValueError as exc:
        return 400, {"error": str(exc)}
    return 200, page.model_dump(mode="json")


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/healthz":
            self._send(200, {"status": "ok"})
            return
        if parsed.path == "/items":
            try:
                status, body = handle_items(parsed.query)
            except ValueError as exc:
                status, body = 400, {"error": str(exc)}
            self._send(status, body)
            return
        self._send(404, {"error": "not found"})

    def _send(self, status: int, body: dict) -> None:
        payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt: str, *args: object) -> None:
        return


def serve(port: int = 8765) -> ThreadingHTTPServer:
    return ThreadingHTTPServer(("127.0.0.1", port), _Handler)


def main() -> None:
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = serve(port)
    print(f"reading-list service listening on http://127.0.0.1:{port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
