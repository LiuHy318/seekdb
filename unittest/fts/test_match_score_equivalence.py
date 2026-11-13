"""Compare MATCH results across binaries, including cached parameter thresholds.

Run `record snapshot.json` on the baseline, then `verify snapshot.json` on the
candidate using the same server data directory. SEEKDB_TEST_PORT and MYSQL_CLIENT
select the server/client. The isolated regression database is recreated each run.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def execute(sql):
    client = os.environ.get('MYSQL_CLIENT', str(Path(__file__).resolve().parents[2] /
                            'deps/3rd/u01/obclient/bin/obclient'))
    result = subprocess.run(
        [client, '-h127.0.0.1', '-P' + os.environ.get('SEEKDB_TEST_PORT', '10105'),
         '-uroot', '-N', '-s', '--raw', '--default-character-set=utf8mb4'],
        input='SET NAMES utf8mb4; USE fts_score_regression;\n' + sql,
        text=True, capture_output=True, timeout=120)
    if result.returncode:
        raise AssertionError({'sql': sql, 'error': result.stderr})
    return result.stdout.splitlines()


def snapshot():
    # Bootstrap separately because execute() selects the regression database.
    client = os.environ.get('MYSQL_CLIENT', str(Path(__file__).resolve().parents[2] /
                            'deps/3rd/u01/obclient/bin/obclient'))
    subprocess.run([client, '-h127.0.0.1', '-P' + os.environ.get('SEEKDB_TEST_PORT', '10105'),
                    '-uroot', '-e', 'CREATE DATABASE IF NOT EXISTS fts_score_regression'],
                   check=True, capture_output=True, timeout=30)
    execute("""DROP TABLE IF EXISTS docs;
CREATE TABLE docs(id INT PRIMARY KEY, category INT, body TEXT,
                 FULLTEXT INDEX ft(body) WITH PARSER beng);
INSERT INTO docs VALUES
 (1,1,'database performance audit database'), (2,1,'database'),
 (3,2,'performance audit'), (4,2,'unrelated zebra'), (5,3,''), (6,3,NULL),
 (7,1,'database database database performance'), (8,2,'audit audit'),
 (9,3,'the and is'), (10,2,'Database PERFORMANCE');
""")
    results = []
    for words in ['database performance', 'audit', 'missingtoken', 'the and', '']:
        match = "MATCH(body) AGAINST('" + words + "')"
        queries = [
            f'SELECT COUNT(*) FROM docs WHERE {match}',
            f'SELECT id FROM docs WHERE {match} ORDER BY id',
            f'SELECT id, body FROM docs WHERE {match} AND category=1 ORDER BY id',
            f'SELECT COUNT(*) FROM (SELECT id FROM docs WHERE {match} LIMIT 3) t',
            f'SELECT id, {match} AS score FROM docs WHERE {match} ORDER BY id',
            f'SELECT id FROM docs WHERE {match} ORDER BY {match} DESC, id LIMIT 4',
            f'SELECT SUM({match}), MAX({match}) FROM docs WHERE {match}',
            f'SELECT category, COUNT(*) FROM docs WHERE {match} GROUP BY category ORDER BY category',
            f'SELECT id FROM docs WHERE {match} AND ({match}>0.5 OR category=3) ORDER BY id',
            f"SELECT id FROM docs WHERE {match} AND CAST({match} AS CHAR)='1' ORDER BY id",
            f'SELECT id FROM docs WHERE {match} AND EXISTS(SELECT 1 WHERE docs.category=1) ORDER BY id',
            f'SELECT a.id, b.id FROM docs a JOIN docs b ON a.category=b.category '
            f"WHERE MATCH(a.body) AGAINST('{words}') AND b.id=2 ORDER BY a.id, b.id",
        ]
        for predicate in [f'{match}>0', f'0<{match}', f'{match}>0.1', f'{match}>=0',
                          f'{match}=0', f'NOT ({match})', f'({match})+0>0',
                          f'{match}>0 AND {match}>0.1']:
            queries.append(f'SELECT id FROM docs WHERE {predicate} ORDER BY id')
        for query in queries:
            results.append([query, execute(query + ';')])
    for words in ['+database -audit', 'database performance', '"database performance"']:
        match = "MATCH(body) AGAINST('" + words + "' IN BOOLEAN MODE)"
        query = f'SELECT id, {match} FROM docs WHERE {match} ORDER BY id'
        results.append([query, execute(query + ';')])
    # More than 256 distinct tokens selects TAAT instead of the DAAT merge.
    long_words = 'database performance audit ' + ' '.join(f'absentword{i}' for i in range(300))
    for words in [long_words, ' '.join(f'absentword{i}' for i in range(300))]:
        query = f"SELECT id FROM docs WHERE MATCH(body) AGAINST('{words}') ORDER BY id"
        results.append([query, execute(query + ';')])
    # Reuse a prepared statement with zero/nonzero thresholds in both orders.
    # The optimizer must constrain a no-score plan to its zero threshold.
    for thresholds in [[0, 0.1, 1, 0], [1, 0, 0.1, 0]]:
        query = "PREPARE s FROM 'SELECT id FROM docs WHERE MATCH(body) AGAINST(?) > ? ORDER BY id';\n"
        query += "SET @words='database performance';\n"
        for threshold in thresholds:
            query += f"SET @threshold={threshold}; EXECUTE s USING @words,@threshold;\n"
        query += 'DEALLOCATE PREPARE s;'
        results.append([query, execute(query)])
    query = "PREPARE s FROM 'SELECT id FROM docs WHERE MATCH(body) AGAINST(?) ORDER BY id';\n"
    for words in ['database', long_words, 'missingtoken', 'audit']:
        query += f"SET @words='{words}'; EXECUTE s USING @words;\n"
    query += 'DEALLOCATE PREPARE s;'
    results.append([query, execute(query)])
    execute("""DROP TABLE IF EXISTS composite_docs;
CREATE TABLE composite_docs(id INT, category INT, body TEXT,
 PRIMARY KEY(id,category), FULLTEXT INDEX ft(body) WITH PARSER beng);
INSERT INTO composite_docs SELECT id,category,body FROM docs;""")
    for table in ['docs', 'composite_docs']:
        for words in ['database performance', long_words]:
            query = f"SELECT id,category,body FROM {table} WHERE MATCH(body) AGAINST('{words}') ORDER BY id"
            results.append([query, execute(query + ';')])
    # Cross several storage batches, exercise unsigned ordering, equal IDs in
    # multiple posting lists, exhausted lists and the small-union cutoff.
    execute("""DROP TABLE IF EXISTS union_docs;
CREATE TABLE union_docs(id BIGINT UNSIGNED PRIMARY KEY, body TEXT,
 FULLTEXT INDEX ft(body) WITH PARSER beng);""")
    vocabulary = ['alpha', 'bravo', 'charlie', 'delta', 'echo', 'foxtrot', 'golf', 'hotel', 'india']
    for start in range(0, 9219, 250):
        rows = []
        for i in range(start, min(start + 250, 9219)):
            body = ' '.join(word for j, word in enumerate(vocabulary) if i % (j + 2) == 0)
            doc_id = i if i < 4608 else (1 << 63) + i
            rows.append(f"({doc_id},'{body}')")
        execute('INSERT INTO union_docs VALUES ' + ','.join(rows) + ';')
    for words in ['alpha', 'alpha bravo', ' '.join(vocabulary[:8]), ' '.join(vocabulary),
                  'alpha alpha bravo absentword', 'absentword missingtoken']:
        match = f"MATCH(body) AGAINST('{words}')"
        for query in [f'SELECT COUNT(*),SUM(id) FROM union_docs WHERE {match}',
                      f'SELECT id FROM union_docs WHERE {match} ORDER BY id',
                      f'SELECT id,body FROM union_docs WHERE {match} AND id%97=0 ORDER BY id',
                      f'SELECT id FROM union_docs WHERE {match} ORDER BY id LIMIT 33 OFFSET 1000',
                      f'SELECT id,body FROM union_docs WHERE {match} AND id%3=0 ORDER BY id DESC LIMIT 31',
                      f'SELECT COUNT(*) FROM (SELECT id FROM union_docs WHERE {match} LIMIT 3000 OFFSET 900) t']:
            results.append([query, execute(query + ';')])
    # Reusing a prepared plan must discard partially/fully consumed windows.
    query = "PREPARE u FROM 'SELECT COUNT(*),SUM(id) FROM union_docs WHERE MATCH(body) AGAINST(?)';\n"
    for words in ['alpha bravo', ' '.join(vocabulary[:8]), 'absentword', 'alpha',
                  ' '.join(vocabulary), 'alpha bravo']:
        query += f"SET @words='{words}'; EXECUTE u USING @words;\n"
    query += 'DEALLOCATE PREPARE u;'
    results.append([query, execute(query)])
    # The storage scan still owns visibility and filtering, including writes
    # in the current transaction and rollback to the original posting lists.
    query = """BEGIN;
UPDATE union_docs SET body='absentword' WHERE id%97=0;
DELETE FROM union_docs WHERE id%101=0;
SELECT COUNT(*),SUM(id) FROM union_docs WHERE MATCH(body) AGAINST('alpha bravo');
ROLLBACK;
SELECT COUNT(*),SUM(id) FROM union_docs WHERE MATCH(body) AGAINST('alpha bravo');"""
    results.append([query, execute(query)])
    execute("UPDATE docs SET body='database audit' WHERE id=4; DELETE FROM docs WHERE id=1;")
    query = "SELECT id FROM docs WHERE MATCH(body) AGAINST('database performance') ORDER BY id"
    results.append([query, execute(query + ';')])
    return results


def check_plans():
    match = "MATCH(body) AGAINST('database performance')"
    for query, need_score in [
        (f'SELECT COUNT(*) FROM docs WHERE {match}', False),
        (f'SELECT id FROM docs WHERE {match} ORDER BY id', False),
        (f'SELECT id FROM docs WHERE {match}', True),
        (f'SELECT id, {match}>0 FROM docs WHERE {match} ORDER BY id', True),
        (f'SELECT id FROM docs WHERE {match} ORDER BY id FOR UPDATE', True),
        (f'SELECT id, {match} FROM docs WHERE {match}', True),
        (f'SELECT id FROM docs WHERE {match} ORDER BY {match} DESC LIMIT 3', True),
        (f'SELECT COUNT(*) FROM docs WHERE {match}>0.1', True),
        ("SELECT COUNT(*) FROM docs WHERE MATCH(body) AGAINST('+database -audit' IN BOOLEAN MODE)", True),
    ]:
        query = query.replace('SELECT ', 'SELECT /*+ INDEX(docs ft) */ ', 1)
        plan = '\n'.join(execute('EXPLAIN ' + query + ';'))
        expected = 'calc_relevance=' + str(need_score).lower()
        uses_scoring_lookup = need_score and 'has_functional_lookup=true' in plan
        assert expected in plan or uses_scoring_lookup, {'query': query, 'expected': expected, 'plan': plan}


def check_count_limit():
    # Isolate this data so ordering mistakes change COUNT(nullable), SUM and
    # filtered counts, while COUNT(*) can safely avoid sorting before LIMIT.
    execute("""DROP TABLE IF EXISTS count_rows;
CREATE TABLE count_rows(id INT PRIMARY KEY, nullable_value INT);
INSERT INTO count_rows VALUES (1,1),(2,2),(3,3),(4,NULL),(5,NULL);""")
    cases = [
        ('SELECT COUNT(*) FROM (SELECT id FROM count_rows ORDER BY id DESC LIMIT 3) t', ['3']),
        ('SELECT COUNT(*) FROM (SELECT id FROM count_rows ORDER BY id DESC LIMIT 3 OFFSET 4) t', ['1']),
        ('SELECT COUNT(*) FROM (SELECT id FROM count_rows ORDER BY id DESC LIMIT 0) t', ['0']),
        ('SELECT COUNT(*) FROM (SELECT id FROM count_rows ORDER BY id DESC LIMIT 20 OFFSET 20) t', ['0']),
        ('SELECT COUNT(nullable_value) FROM (SELECT nullable_value FROM count_rows ORDER BY id DESC LIMIT 3) t', ['1']),
        ('SELECT SUM(id) FROM (SELECT id FROM count_rows ORDER BY id DESC LIMIT 3) t', ['12']),
        ('SELECT COUNT(*) FROM (SELECT id FROM count_rows ORDER BY id DESC LIMIT 3) t WHERE id<3', ['0']),
        ('SELECT COUNT(*) FROM (SELECT DISTINCT nullable_value FROM count_rows ORDER BY nullable_value DESC LIMIT 20) t', ['4']),
        ('SELECT COUNT(*) FROM (SELECT nullable_value FROM count_rows GROUP BY nullable_value ORDER BY nullable_value DESC LIMIT 20) t', ['4']),
    ]
    for query, expected in cases:
        assert execute(query + ';') == expected, query
    count_plan = '\n'.join(execute('EXPLAIN SELECT COUNT(*) FROM '
                                  '(SELECT nullable_value FROM count_rows ORDER BY nullable_value DESC LIMIT 3) t;'))
    assert 'sort_keys(' not in count_plan, count_plan
    fts_plan = '\n'.join(execute("EXPLAIN SELECT COUNT(*) FROM "
                                "(SELECT id FROM docs WHERE MATCH(body) AGAINST('database performance') LIMIT 3) t;"))
    assert 'calc_relevance=false' in fts_plan and 'sort_keys(' not in fts_plan, fts_plan


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['record', 'verify'])
    parser.add_argument('path', type=Path)
    parser.add_argument('--check-plans', action='store_true', help='Assert candidate scoring decisions')
    parser.add_argument('--queries-from', type=Path,
                        help='Replay a saved query list against existing data without rebuilding fixtures')
    args = parser.parse_args()
    if args.queries_from:
        saved = json.loads(args.queries_from.read_text())
        actual = [[query, execute(query if query.rstrip().endswith(';') else query + ';')]
                  for query, _ in saved]
    else:
        actual = snapshot()
    if args.mode == 'record':
        args.path.write_text(json.dumps(actual, ensure_ascii=False, indent=2))
    else:
        expected = json.loads(args.path.read_text())
        assert len(expected) == len(actual), (len(expected), len(actual))
        for before, after in zip(expected, actual):
            assert before == after, {'before': before, 'after': after}
    print(f'{args.mode}: {len(actual)} MATCH result groups checked')
    if args.check_plans:
        check_plans()
        print('9 optimizer scoring decisions checked')
        check_count_limit()
        print('9 COUNT/LIMIT cardinality and ordering cases checked')
