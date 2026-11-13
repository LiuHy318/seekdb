"""Compare populated-table FTS builds with an independently recorded binary.

Use record snapshot.json on the baseline, then verify snapshot.json on the
candidate. Each run recreates only fts_build_regression. No golden test is edited.
SEEKDB_TEST_PORT and MYSQL_CLIENT select the local instance.
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
        input='SET NAMES utf8mb4; SET ob_query_timeout=120000000;\n' + sql,
        text=True, capture_output=True, timeout=150)
    if result.returncode:
        raise AssertionError({'sql_start': sql[:200], 'error': result.stderr})
    return result.stdout.splitlines()


def literal(text):
    return "CONVERT(UNHEX('" + text.encode().hex() + "') USING utf8mb4)"


def alphabetic(number):
    chars = []
    for _ in range(4):
        chars.append(chr(ord('a') + number % 26))
        number //= 26
    return ''.join(chars)


def snapshot():
    execute('DROP DATABASE IF EXISTS fts_build_regression; '
            'CREATE DATABASE fts_build_regression;')
    results = []
    # More than one output batch, then empty rows and a short document. The
    # repeated long row also exercises out-of-row LOB materialization.
    long_text = ' '.join('uniqueword' + alphabetic(i) for i in range(2400))
    texts = [long_text, '', None, 'the and is',
             'database audit database 中文 全文索引', 'Database AUDIT café CAFÉ',
             long_text * 3, 'tailmarker database', None, '']
    cases = [('beng_ci', 'beng', 'utf8mb4_general_ci', 'primary'),
             ('beng_bin', 'beng', 'utf8mb4_bin', 'primary'),
             ('ik_ci', 'ik', 'utf8mb4_general_ci', 'primary'),
             ('space_bin', 'space', 'utf8mb4_bin', 'primary'),
             ('ngram_ci', 'ngram', 'utf8mb4_general_ci', 'primary'),
             ('heap_beng', 'beng', 'utf8mb4_general_ci', 'heap'),
             ('part_beng', 'beng', 'utf8mb4_general_ci', 'partition')]
    words = ['database', 'audit database', 'tailmarker', 'uniquewordaaaa',
             'uniquewordhoaa', '全文索引', 'missingtoken']
    for name, parser, collation, layout in cases:
        table = 'fts_build_regression.' + name
        pk = '' if layout == 'heap' else ' PRIMARY KEY'
        partition = ' PARTITION BY HASH(id) PARTITIONS 4' if layout == 'partition' else ''
        execute(f'CREATE TABLE {table}(id INT{pk}, body LONGTEXT COLLATE {collation})'
                f' DEFAULT CHARSET=utf8mb4{partition};')
        rows = ','.join(f'({i + 1},' + ('NULL' if text is None else literal(text)) + ')'
                        for i, text in enumerate(texts))
        execute(f'INSERT INTO {table} VALUES {rows};')
        execute(f'ALTER TABLE {table} ADD FULLTEXT INDEX ft(body) WITH PARSER {parser};')

        def collect(stage):
            for word in words:
                match = f'MATCH(body) AGAINST({literal(word)})'
                for query in [f'SELECT id, ROUND({match}, 9) FROM {table} WHERE {match} ORDER BY id',
                              f'SELECT COUNT(*) FROM {table} WHERE {match}',
                              f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} LIMIT 3) x']:
                    results.append([name, stage, query, execute(query + ';')])

        collect('build')
        execute(f"UPDATE {table} SET body={literal('changedmarker audit')} WHERE id=1; "
                f'DELETE FROM {table} WHERE id=5; '
                f'INSERT INTO {table} VALUES(11,{literal("tailmarker audit")});')
        collect('dml')
        execute(f'ALTER TABLE {table} DROP INDEX ft; '
                f'ALTER TABLE {table} ADD FULLTEXT INDEX ft(body) WITH PARSER {parser};')
        collect('rebuild')
        print(f'{name}: build/DML/rebuild checked', flush=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['record', 'verify'])
    parser.add_argument('snapshot', type=Path)
    args = parser.parse_args()
    current = snapshot()
    if args.mode == 'record':
        args.snapshot.write_text(json.dumps(current, ensure_ascii=False, indent=2) + '\n')
    else:
        expected = json.loads(args.snapshot.read_text())
        if len(expected) != len(current):
            raise AssertionError((len(expected), len(current)))
        for old, new in zip(expected, current):
            if old != new:
                raise AssertionError({'expected': old, 'actual': new})
    print(f'{args.mode}: {len(current)} populated-build result groups checked', flush=True)


if __name__ == '__main__':
    main()
