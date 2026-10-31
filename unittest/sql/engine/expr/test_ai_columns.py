"""AI COLUMN transaction and publication regressions against a local server.

Run with MYSQL set, using unittest discovery in this directory. No model
credentials or external service are needed.
"""

from collections import Counter
import json
import threading
import time
import unittest

from test_ai_functions import AIFunctionSQLTest, MockHandler, literal, vector


class ColumnMockHandler(MockHandler):
    counts = Counter()

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        prompt = body.get("messages", [{"content": ""}])[-1]["content"]
        text = prompt.split("\nReturn exactly", 1)[0]
        with self.lock:
            self.requests.append((self.path, body, dict(self.headers)))
            self.counts[(self.path, text)] += 1
            attempt = self.counts[(self.path, text)]
        if text == "slow":
            time.sleep(0.6)
        if text == "flaky" and attempt <= 2:
            self.send_error(503)
            return
        if self.path.endswith("embed"):
            inputs = body["input"]
            inputs = inputs if isinstance(inputs, list) else [inputs]
            response = {"data": [{"index": i, "embedding": vector(value, body.get("dimensions", 4))}
                                 for i, value in reversed(list(enumerate(inputs)))]}
        else:
            content = "invalid JSON" if text == "broken" else json.dumps({"source": text})
            response = {"choices": [{"message": {"content": content}}]}
        encoded = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


class ColumnSQLTest(AIFunctionSQLTest):
    def setUp(self):
        super().setUp()
        self.server.RequestHandlerClass = ColumnMockHandler
        with ColumnMockHandler.lock:
            ColumnMockHandler.counts.clear()
        self.text_model = self.model("completion", "/column/complete")
        self.embed_model = self.model("dense_embedding", "/column/embed")
        self.table = "test." + self.prefix
        self.sql("CREATE TABLE " + self.table + "(id BIGINT PRIMARY KEY,title TEXT,content TEXT,"
                 "tags JSON AI COLUMN(AI_COMPLETE(" + self.text_model + ",content)) WITH(max_retries=2),"
                 "emb VECTOR(4) AI COLUMN(AI_EMBED(" + self.embed_model + ",content)))")
        self.addCleanup(self.sql, "DROP TABLE " + self.table)

    def eventually(self, query, expected, timeout=20):
        deadline = time.monotonic() + timeout
        actual = None
        while time.monotonic() < deadline:
            actual = self.sql(query)
            if actual == expected:
                return
            time.sleep(0.05)
        self.fail("expected " + repr(expected) + ", got " + repr(actual))

    def source(self, identifier=1):
        return "SELECT JSON_UNQUOTE(JSON_EXTRACT(tags,'$.source')) FROM " + self.table + " WHERE id=" + str(identifier)

    def test_transactional_queue_dependencies_and_direct_writes(self):
        with ColumnMockHandler.lock:
            before = len(ColumnMockHandler.requests)
        self.sql("BEGIN; INSERT INTO " + self.table + "(id,content) VALUES(99,'rollback'); ROLLBACK")
        time.sleep(0.3)
        self.assertEqual("0", self.sql("SELECT COUNT(*) FROM " + self.table))
        self.assertEqual(before, len(ColumnMockHandler.requests))
        self.sql("INSERT INTO " + self.table + "(id,content) VALUES(1,'alpha'),(2,'beta')")
        self.eventually(self.source(), "alpha")
        self.eventually(self.source(2), "beta")
        actual = json.loads(self.sql("SELECT emb FROM " + self.table + " WHERE id=1"))
        self.assertEqual(4, len(actual))
        for expected, value in zip(vector("alpha"), actual):
            self.assertAlmostEqual(expected, value, delta=1e-6)
        with ColumnMockHandler.lock:
            before = len(ColumnMockHandler.requests)
        self.sql("UPDATE " + self.table + " SET title='unrelated'")
        time.sleep(0.3)
        self.assertEqual(before, len(ColumnMockHandler.requests))
        self.sql("BEGIN; UPDATE " + self.table + " SET content='rollback'; ROLLBACK")
        self.assertEqual("alpha", self.sql(self.source()))
        self.sql("UPDATE " + self.table + " SET id=3,content='gamma' WHERE id=1")
        self.eventually(self.source(3), "gamma")
        for query in ["UPDATE " + self.table + " SET tags=JSON_OBJECT()",
                      "INSERT INTO " + self.table + "(id,tags) VALUES(9,JSON_OBJECT())",
                      "INSERT INTO " + self.table + "(id,tags) SELECT 9,JSON_OBJECT()"]:
            self.sql(query, succeeds=False)

    def test_retry_state_pseudocolumn_and_column_level_strict_read(self):
        self.sql("INSERT INTO " + self.table + "(id,content) VALUES(1,'alpha'),(2,'broken')")
        self.eventually(self.source(), "alpha")
        self.eventually("SELECT COUNT(*) FROM " + self.table + " WHERE id=2 AND emb IS NOT NULL", "1")
        self.sql("SET SESSION ai_column_strict_read=ON; SELECT tags FROM " + self.table + " WHERE id=1", succeeds=False)
        self.assertEqual("1\n2", self.sql("SET SESSION ai_column_strict_read=ON; SELECT id FROM " + self.table + " ORDER BY id"))
        self.assertNotEqual("NULL", self.sql("SET SESSION ai_column_strict_read=ON; SELECT emb FROM " + self.table + " WHERE id=1"))
        self.sql("ALTER TABLE " + self.table + " RETRY AI COLUMN tags WHERE status='FAILED'")
        self.sql("UPDATE " + self.table + " SET content='recovered' WHERE id=2")
        self.eventually(self.source(2), "recovered")
        self.sql("ALTER TABLE " + self.table + " RETRY AI COLUMN tags WHERE id=1")
        self.eventually(self.source(), "alpha")
        self.sql("ALTER TABLE " + self.table + " RETRY AI COLUMN content WHERE id=1", succeeds=False)
        self.sql("ALTER TABLE " + self.table + " RETRY AI COLUMN tags WHERE id=999")

    def test_latest_request_wins_delete_and_bounded_retry(self):
        self.sql("INSERT INTO " + self.table + "(id,content) VALUES(1,'slow')")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            with ColumnMockHandler.lock:
                if ColumnMockHandler.counts[("/column/complete", "slow")]:
                    break
            time.sleep(0.02)
        self.sql("UPDATE " + self.table + " SET content='latest' WHERE id=1")
        self.eventually(self.source(), "latest")
        time.sleep(0.7)
        self.assertEqual("latest", self.sql(self.source()))
        self.sql("INSERT INTO " + self.table + "(id,content) VALUES(2,'flaky')")
        self.eventually(self.source(2), "flaky")
        with ColumnMockHandler.lock:
            self.assertEqual(3, ColumnMockHandler.counts[("/column/complete", "flaky")])
        self.sql("INSERT INTO " + self.table + "(id,content) VALUES(3,'slow'); DELETE FROM " + self.table + " WHERE id=3")
        time.sleep(1)
        self.assertEqual("0", self.sql("SELECT COUNT(*) FROM " + self.table + " WHERE id=3"))


def load_tests(loader, tests, pattern):
    # Reuse the SQL fixture without running its inherited AI function tests.
    names = [name for name in ColumnSQLTest.__dict__ if name.startswith("test_")]
    return unittest.TestSuite(ColumnSQLTest(name) for name in sorted(names))
