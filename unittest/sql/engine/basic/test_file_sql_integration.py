"""Native server regression checks. Requires SEEKDB_FILE_SQL_TEST_DIR and PyMySQL."""

import csv
import os
from pathlib import Path
import unittest

import pymysql


class FileSQLIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(os.environ['SEEKDB_FILE_SQL_TEST_DIR']).resolve()
        cls.root.mkdir(parents=True, exist_ok=True)
        cls.db = pymysql.connect(host='127.0.0.1', port=2881, user='admin',
                                 password='admin', database='test', autocommit=True)
        with (cls.root / 'records.csv').open('w', encoding='utf8', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['id', 'name', 'score', 'flag', 'day'])
            writer.writerows([(1, '中文,"quoted"\nline', 1.5, 'true', '2026-01-02'),
                              (2, 'second', 2.5, 'false', '2026-01-03'),
                              (3, 'third', 3.5, 'true', '2026-01-04')])
        (cls.root / 'right.jsonl').write_text(
            '{"id":1,"label":"match"}\n{"id":4,"extra":true}', encoding='utf8')

    @classmethod
    def tearDownClass(cls):
        cls.db.close()

    def query(self, sql):
        with self.db.cursor() as cursor:
            cursor.execute(sql)
            return cursor.fetchall()

    def file(self, name):
        return str(self.root / name).replace("'", "''")

    def setUp(self):
        self.query('set session enable_file_sql=ON')

    def test_disabled_and_ordinary_sql(self):
        self.query('set session enable_file_sql=OFF')
        self.assertEqual(((3,),), self.query('select 1+2'))
        with self.assertRaises(pymysql.MySQLError):
            self.query(f"select * from read_csv('{self.file('records.csv')}')")

    def test_projection_aggregation_and_utf8(self):
        rows = self.query(f"select name from read_csv('{self.file('records.csv')}') where id=1")
        self.assertEqual(('中文,"quoted"\nline',), rows[0])
        self.assertEqual(((2, 5, 3.0),), self.query(
            f"select count(*),sum(id),avg(score) from read_csv('{self.file('records.csv')}') where id>1"))
        self.assertEqual(((1, 2),), self.query(
            f"select flag,count(*) from read_csv('{self.file('records.csv')}') "
            'group by flag having count(*)>1 order by flag limit 1'))

    def test_cross_format_joins(self):
        left = f"read_csv('{self.file('records.csv')}') a"
        right = f"read_ndjson('{self.file('right.jsonl')}') b"
        self.assertEqual(((1, 'match'),), self.query(
            f'select a.id,b.label from {left} inner join {right} on a.id=b.id'))
        self.assertEqual(((1, 'match'), (2, None), (3, None)), self.query(
            f'select a.id,b.label from {left} left join {right} on a.id=b.id order by a.id'))

    def test_describe_glob_and_copy(self):
        self.assertEqual('id', self.query(
            f"describe select * from read_csv('{self.file('records.csv')}')")[0][0])
        self.assertEqual(1, len(self.query(f"select file from glob('{self.file('*.csv')}')")))
        output = self.root / 'roundtrip.txt'
        output.unlink(missing_ok=True)
        try:
            self.query(f"copy (select id,name from read_csv('{self.file('records.csv')}') order by id) "
                       f"to '{self.file('roundtrip.txt')}' (format csv, header, nullstr 'NULL')")
            self.assertEqual(((1,), (2,), (3,)), self.query(
                f"select id from read_csv('{self.file('roundtrip.txt')}') order by id"))
            original = output.read_bytes()
            with self.assertRaises(pymysql.MySQLError):
                self.query(f"copy (select 9) to '{self.file('roundtrip.txt')}' (format csv)")
            self.assertEqual(original, output.read_bytes())
        finally:
            output.unlink(missing_ok=True)

    def test_malformed_json_and_sample_rejected(self):
        (self.root / 'nested.jsonl').write_text('{"nested":{"x":1}}', encoding='utf8')
        with self.assertRaises(pymysql.MySQLError):
            self.query(f"select * from read_json('{self.file('nested.jsonl')}')")
        with self.assertRaises(pymysql.MySQLError):
            self.query(f"select * from read_csv('{self.file('records.csv')}',sample_size=1)")

    def test_record_budget_covers_total_csv_row_and_jsonl(self):
        # Each field is individually under the field limit, while the combined
        # CSV row exceeds the record budget. Build fixtures without huge strings.
        oversized = self.root / 'oversized.txt'
        try:
            with oversized.open('w') as f:
                f.write('a,b,c\n')
                for field in range(3):
                    if field:
                        f.write(',')
                    for _ in range(6):
                        f.write('x' * (1024 * 1024))
                f.write('\n')
            with self.assertRaises(pymysql.MySQLError):
                self.query(f"select * from read_csv('{self.file('oversized.txt')}')")
            with oversized.open('w') as f:
                f.write('{"value":"')
                for _ in range(18):
                    f.write('x' * (1024 * 1024))
                f.write('"}')
            with self.assertRaises(pymysql.MySQLError):
                self.query(f"select * from read_json('{self.file('oversized.txt')}')")
            self.assertEqual(((42,),), self.query('select 42'))
        finally:
            oversized.unlink(missing_ok=True)


if __name__ == '__main__':
    unittest.main()
