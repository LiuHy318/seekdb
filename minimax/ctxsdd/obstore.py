"""OceanBase（seekdb）存储层：工程资料分块入库 + 全文检索。"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any, Iterable, Sequence

from ctxsdd.config import Config

TABLE = "mm_doc"

SCHEMA_SQL = f"""
CREATE TABLE IF NOT EXISTS {TABLE} (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    doc_id VARCHAR(191) NOT NULL,
    kind VARCHAR(16) NOT NULL,
    path VARCHAR(512) NOT NULL,
    symbol VARCHAR(255) NOT NULL DEFAULT '',
    heading VARCHAR(255) NOT NULL DEFAULT '',
    line_from INT NOT NULL DEFAULT 0,
    line_to INT NOT NULL DEFAULT 0,
    content MEDIUMTEXT NOT NULL,
    content_sha CHAR(64) NOT NULL,
    ingested_at VARCHAR(40) NOT NULL,
    UNIQUE KEY uk_doc_id (doc_id),
    FULLTEXT KEY ft_content (content) WITH PARSER ngram
) DEFAULT CHARSET = utf8mb4
"""


@dataclass(frozen=True)
class DocChunk:
    doc_id: str
    kind: str
    path: str
    content: str
    symbol: str = ""
    heading: str = ""
    line_from: int = 0
    line_to: int = 0

    @property
    def content_sha(self) -> str:
        import hashlib
        return hashlib.sha256(self.content.encode("utf-8")).hexdigest()


@dataclass(frozen=True)
class Hit:
    doc_id: str
    kind: str
    path: str
    symbol: str
    heading: str
    line_from: int
    line_to: int
    content: str
    score: float
    ft_score: float
    like_hits: int

    def locator(self, *, with_content: bool = False) -> dict[str, Any]:
        import hashlib
        loc = {
            "doc_id": self.doc_id, "kind": self.kind, "path": self.path,
            "symbol": self.symbol, "heading": self.heading,
            "line_from": self.line_from, "line_to": self.line_to,
            "content_sha": hashlib.sha256(self.content.encode("utf-8")).hexdigest(),
            "score": round(self.score, 6),
        }
        if with_content:
            loc["content"] = self.content
        return loc


class OceanBaseStore:
    """对 mm_doc 的薄封装。"""

    def __init__(self, config: Config) -> None:
        self.config = config
        self._log_path = config.record("ob_query_log.jsonl")

    def _connect(self, *, with_database: bool = True):
        import pymysql
        kwargs: dict[str, Any] = {
            "host": self.config.ob_host, "port": self.config.ob_port,
            "user": self.config.ob_user, "password": self.config.ob_password,
            "charset": "utf8mb4", "autocommit": True, "connect_timeout": 10,
        }
        if with_database:
            kwargs["database"] = self.config.ob_database
        return pymysql.connect(**kwargs)

    def server_info(self) -> dict[str, Any]:
        with self._connect(with_database=False) as conn, conn.cursor() as cur:
            cur.execute("SELECT VERSION(), @@version_comment")
            version, comment = cur.fetchone()
        return {"server_version": version, "version_comment": comment}

    def ensure_schema(self) -> dict[str, Any]:
        db = self.config.ob_database
        with self._connect(with_database=False) as conn, conn.cursor() as cur:
            cur.execute(f"CREATE DATABASE IF NOT EXISTS `{db}` DEFAULT CHARACTER SET utf8mb4")
        with self._connect() as conn, conn.cursor() as cur:
            cur.execute(SCHEMA_SQL)
            cur.execute(f"SHOW CREATE TABLE {TABLE}")
            ddl = cur.fetchone()[1]
        return {"database": db, "table": TABLE, "ddl": ddl}

    def reset(self) -> None:
        with self._connect() as conn, conn.cursor() as cur:
            cur.execute(f"DROP TABLE IF EXISTS {TABLE}")

    def count(self) -> int:
        with self._connect() as conn, conn.cursor() as cur:
            cur.execute(f"SELECT COUNT(*) FROM {TABLE}")
            return int(cur.fetchone()[0])

    def spec_rows(self) -> list[tuple[str, str]]:
        with self._connect() as conn, conn.cursor() as cur:
            cur.execute(f"SELECT path, content_sha FROM {TABLE} WHERE kind='spec' ORDER BY path, content_sha")
            return [(row[0], row[1]) for row in cur.fetchall()]

    def replace_all(self, chunks: Sequence[DocChunk]) -> dict[str, int]:
        counts: dict[str, int] = {}
        now = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        with self._connect() as conn, conn.cursor() as cur:
            cur.execute(f"DELETE FROM {TABLE}")
            sql = (
                f"INSERT INTO {TABLE} "
                "(doc_id, kind, path, symbol, heading, line_from, line_to, content, content_sha, ingested_at) "
                "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)"
            )
            rows = [
                (c.doc_id, c.kind, c.path, c.symbol, c.heading,
                 c.line_from, c.line_to, c.content, c.content_sha, now)
                for c in chunks
            ]
            for start in range(0, len(rows), 100):
                cur.executemany(sql, rows[start:start + 100])
        for c in chunks:
            counts[c.kind] = counts.get(c.kind, 0) + 1
        return counts

    def _log_query(self, record: dict[str, Any]) -> None:
        import json
        with self._log_path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

    def search(self, terms: Iterable[str], *, kinds: Sequence[str] | None = None,
               limit: int = 8) -> list[Hit]:
        term_list = [t.strip() for t in dict.fromkeys(terms) if t.strip()]
        if not term_list:
            return []
        kinds = list(kinds or ["spec", "code", "test"])
        placeholders = ", ".join(["%s"] * len(kinds))
        phrase = " ".join(term_list)
        like_or = " OR ".join(["(content LIKE %s)"] * len(term_list))
        like_params = [f"%{t}%" for t in term_list]

        ft_sql = (
            "SELECT doc_id, kind, path, symbol, heading, line_from, line_to, content, "
            "MATCH(content) AGAINST (%s IN NATURAL LANGUAGE MODE) AS ft_score "
            f"FROM {TABLE} WHERE kind IN ({placeholders}) "
            "AND MATCH(content) AGAINST (%s IN NATURAL LANGUAGE MODE) "
            "ORDER BY ft_score DESC, doc_id ASC LIMIT %s"
        )
        like_sql = (
            "SELECT doc_id, kind, path, symbol, heading, line_from, line_to, content, "
            f"({' + '.join(['(content LIKE %s)'] * len(term_list))}) AS like_hits "
            f"FROM {TABLE} WHERE kind IN ({placeholders}) AND ({like_or}) "
            "ORDER BY like_hits DESC, doc_id ASC LIMIT %s"
        )

        merged: dict[str, dict[str, Any]] = {}
        started = time.perf_counter()
        with self._connect() as conn, conn.cursor() as cur:
            cur.execute(ft_sql, [phrase, *kinds, phrase, limit * 4])
            ft_rows = cur.fetchall()
            cur.execute(like_sql, [*like_params, *kinds, *like_params, limit * 4])
            like_rows = cur.fetchall()
        elapsed_ms = round((time.perf_counter() - started) * 1000, 2)

        for row in ft_rows:
            e = merged.setdefault(row[0], {"row": row, "ft_score": 0.0, "like_hits": 0})
            e["ft_score"] = max(e["ft_score"], float(row[8] or 0.0))
        for row in like_rows:
            e = merged.setdefault(row[0], {"row": row, "ft_score": 0.0, "like_hits": 0})
            e["like_hits"] = max(e["like_hits"], int(row[8] or 0))

        hits: list[Hit] = []
        for doc_id, e in merged.items():
            row = e["row"]
            hits.append(Hit(
                doc_id=doc_id, kind=row[1], path=row[2], symbol=row[3] or "",
                heading=row[4] or "", line_from=int(row[5] or 0), line_to=int(row[6] or 0),
                content=row[7], score=e["ft_score"] + 0.05 * e["like_hits"],
                ft_score=e["ft_score"], like_hits=e["like_hits"],
            ))
        hits.sort(key=lambda h: (-h.score, h.doc_id))
        hits = hits[:limit]

        self._log_query({
            "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "terms": term_list, "elapsed_ms": elapsed_ms,
            "fulltext_rows": len(ft_rows), "like_rows": len(like_rows),
            "returned": [h.locator() for h in hits],
        })
        return hits
