"""Native SQL regressions for file scans and CSV export.

Run against a dedicated local database with MYSQL set to its client command
and FILE_SQL_ROOT set to the directory allowed by secure_file_priv.
"""

import base64
import csv
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
import uuid


def literal(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "''") + "'"


@unittest.skipUnless(os.environ.get("MYSQL") and os.environ.get("FILE_SQL_ROOT"),
                     "set MYSQL and FILE_SQL_ROOT for a dedicated local test instance")
class FileSQLTest(unittest.TestCase):
    def sql(self, text, succeeds=True, enabled=True):
        prefix = "SET SESSION enable_file_sql=ON;" if enabled else ""
        result = subprocess.run(shlex.split(os.environ["MYSQL"]) + [
            "--batch", "--skip-column-names", "--raw", "--default-character-set=utf8mb4",
            "-e", prefix + text], capture_output=True, encoding="utf-8", timeout=60)
        if succeeds:
            self.assertEqual(0, result.returncode, result.stderr)
        else:
            self.assertNotEqual(0, result.returncode, result.stdout)
        return result.stdout.rstrip("\n")

    def setUp(self):
        root = Path(os.environ["FILE_SQL_ROOT"]).resolve(strict=True)
        self.allowed_root = root
        temporary = tempfile.TemporaryDirectory(prefix="file_sql_", dir=root)
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve(strict=True)
        self.assertEqual(root, self.directory.parent)
        self.csv = self.directory / "records.csv"
        with self.csv.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(["id", "name", "score", "active", "day"])
            writer.writerows([[1, "你好,\n世界", 1.5, "true", "2024-02-29"],
                              [2, 'a"b', 2, "false", ""],
                              [3, "third", 3.5, "true", "2026-10-31"]])
        self.json = self.directory / "labels.jsonl"
        with self.json.open("w", encoding="utf-8") as stream:
            for record in [{"id": 1, "label": "one"}, {"id": 3, "label": "three"}]:
                stream.write(json.dumps(record) + "\n")

    def test_native_join_group_filter_sort_and_limit(self):
        source = "read_csv(" + literal(self.csv) + ")"
        labels = "read_ndjson(" + literal(self.json) + ")"
        self.assertEqual("1\tone\n2\tNULL\n3\tthree", self.sql(
            "SELECT f.id,j.label FROM " + source + " f LEFT JOIN " + labels +
            " j ON f.id=j.id ORDER BY f.id"))
        self.assertEqual("1\n3", self.sql("SELECT f.id FROM " + source +
            " f INNER JOIN " + labels + " j ON f.id=j.id ORDER BY f.id"))
        self.assertEqual("0\t1\t2\n1\t2\t4", self.sql(
            "SELECT active,COUNT(*),SUM(id) FROM " + source +
            " GROUP BY active HAVING COUNT(*)>0 ORDER BY active"))
        self.assertEqual("3\t3.5\n2\t2", self.sql(
            "SELECT id,score FROM " + source + " WHERE score>=2 ORDER BY score DESC LIMIT 2"))
        self.assertEqual("3", self.sql("SELECT COUNT(*) FROM " + source))

    def test_describe_metadata_and_file_discovery(self):
        metadata = [line.split("\t")[:3] for line in self.sql(
            "DESCRIBE SELECT * FROM read_csv(" + literal(self.csv) + ")").splitlines()]
        self.assertEqual([["id", "BIGINT", "NO"], ["name", "VARCHAR", "NO"],
                          ["score", "DOUBLE", "NO"], ["active", "BOOLEAN", "NO"],
                          ["day", "DATE", "YES"]], metadata)
        self.assertEqual("labels.jsonl\nrecords.csv", self.sql(
            "SELECT SUBSTRING_INDEX(file,'/',-1) FROM glob(" +
            literal(self.directory / "*") + ") ORDER BY file"))

    def test_different_paths_keep_their_own_dynamic_schema(self):
        first = self.directory / "first.csv"
        second = self.directory / "second.csv"
        first.write_text("id\n7\n", encoding="utf-8")
        second.write_text("label,value\nsecond,9\n", encoding="utf-8")
        for _ in range(2):
            self.assertEqual("7", self.sql("SELECT * FROM read_csv(" + literal(first) + ")"))
            self.assertEqual("second\t9", self.sql("SELECT * FROM read_csv(" + literal(second) + ")"))
        self.sql("SELECT * FROM read_csv(" + literal(first) + ",delim=',',delimiter=';')", succeeds=False)

    def test_parquet_projection_aggregate_and_filter(self):
        fixture = Path(__file__).resolve().parents[4] / (
            "tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64")
        source = self.directory / "simple.parquet"
        source.write_bytes(base64.b64decode(fixture.read_bytes()))
        relation = "read_parquet(" + literal(source) + ")"
        self.assertEqual("7\t0\t6\t21", self.sql(
            "SELECT COUNT(*),MIN(c1),MAX(c1),SUM(c1) FROM " + relation))
        self.assertEqual("6\n5", self.sql("SELECT c1 FROM " + relation +
                         " WHERE c1>3 ORDER BY c1 DESC LIMIT 2"))

    def test_copy_roundtrip_union_and_existing_destination(self):
        output = self.directory / "output.csv"
        source = "read_csv(" + literal(self.csv) + ")"
        self.sql("COPY (SELECT id,name,day FROM " + source + " ORDER BY id) TO " +
                 literal(output) + " (FORMAT csv, HEADER, DELIMITER ';', NULLSTR 'NULL')")
        with output.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.reader(stream, delimiter=";"))
        self.assertEqual(["id", "name", "day"], rows[0])
        self.assertEqual(["1", "你好,\n世界", "2024-02-29"], rows[1])
        self.assertEqual(["2", 'a"b', "NULL"], rows[2])
        self.assertEqual("1\t0\n2\t1\n3\t0", self.sql(
            "SELECT id,day IS NULL FROM read_csv(" + literal(output) +
            ",delimiter=';',nullstr='NULL') ORDER BY id"))
        original = output.read_bytes()
        self.sql("COPY (SELECT 9 AS id) TO " + literal(output), succeeds=False)
        self.assertEqual(original, output.read_bytes())
        union = self.directory / "union.csv"
        self.sql("COPY (SELECT 1 AS value UNION ALL SELECT 2 ORDER BY value) TO " +
                 literal(union) + " (HEADER)")
        self.assertEqual("1\n2", self.sql("SELECT value FROM read_csv(" + literal(union) + ") ORDER BY value"))

    def test_default_disabled_security_and_failed_copy_cleanup(self):
        self.sql("SELECT * FROM read_csv(" + literal(self.csv) + ")", succeeds=False, enabled=False)
        if self.allowed_root.parent != self.allowed_root:
            outside = tempfile.TemporaryDirectory(prefix="file_sql_outside_", dir=self.allowed_root.parent)
            self.addCleanup(outside.cleanup)
            outside_path = Path(outside.name) / "outside.csv"
            outside_path.write_text("id\n9\n", encoding="utf-8")
            self.sql("SELECT * FROM read_csv(" + literal(outside_path) + ")", succeeds=False)
            link = self.directory / "escape.csv"
            link.symlink_to(outside_path)
            self.sql("SELECT * FROM read_csv(" + literal(link) + ")", succeeds=False)
            escaped_output = Path(outside.name) / "export.csv"
            self.sql("COPY (SELECT 1 AS id) TO " + literal(escaped_output), succeeds=False)
            self.assertFalse(escaped_output.exists())
        before = sorted(path.name for path in self.directory.iterdir())
        output = self.directory / "failed.csv"
        # A scalar subquery returning three rows fails after execution starts.
        self.sql("COPY (SELECT (SELECT id FROM read_csv(" + literal(self.csv) + "))) TO " +
                 literal(output), succeeds=False)
        self.assertFalse(output.exists())
        self.assertEqual(before, sorted(path.name for path in self.directory.iterdir()))
        table = "test.file_copy_" + uuid.uuid4().hex[:12]
        self.sql("CREATE DATABASE IF NOT EXISTS test; CREATE TABLE " + table + "(id INT PRIMARY KEY)")
        self.addCleanup(self.sql, "DROP TABLE " + table)
        self.sql("INSERT INTO " + table + " VALUES (7),(8)")
        self.sql("COPY " + table + " TO " + literal(output) + " (HEADER)")
        self.assertEqual("7\n8", self.sql("SELECT id FROM read_csv(" + literal(output) + ") ORDER BY id"))


if __name__ == "__main__":
    unittest.main()
