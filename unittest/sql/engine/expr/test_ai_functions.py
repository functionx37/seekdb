"""SQL regressions against a local deterministic HTTP service.

Run with MYSQL='mysql -h127.0.0.1 -P2881 -uroot' python3 -m unittest
discover -s unittest/sql/engine/expr -p test_ai_functions.py -v.
The database must run on the same host as this test's HTTP service.
"""

import hashlib
import json
import os
import shlex
import subprocess
import threading
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def literal(value):
    return "'" + str(value).replace("\\", "\\\\").replace("'", "''") + "'"


def vector(text, dimension=4):
    digest = hashlib.sha256(text.encode()).digest()
    return [digest[i % len(digest)] / 256 for i in range(dimension)]


class MockHandler(BaseHTTPRequestHandler):
    requests = []
    fault = None
    lock = threading.Lock()

    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        with self.lock:
            self.requests.append((self.path, body, dict(self.headers)))
            fault = self.fault
        if self.path.endswith("embed"):
            inputs = body["input"]
            inputs = inputs if isinstance(inputs, list) else [inputs]
            dimension = body.get("dimensions", 4)
            values = [vector(text, dimension) for text in inputs]
            if self.path.startswith("/ollama"):
                response = {"embeddings": values}
            else:
                data = [{"index": i, "embedding": value} for i, value in enumerate(values)]
                if fault == "duplicate_index" and len(data) > 1:
                    data[1]["index"] = data[0]["index"]
                if fault == "wrong_dimension":
                    data[0]["embedding"].append(0)
                response = {"data": list(reversed(data))}
        elif self.path.endswith("rerank"):
            # Deliberately unsorted, with ties. The engine must validate and sort.
            response = {"results": [{"index": i, "relevance_score": len(text) % 3}
                                     for i, text in reversed(list(enumerate(body["documents"])))]}
        else:
            prompt = body.get("prompt")
            if prompt is None:
                prompt = body["messages"][-1]["content"]
            response = {"response": prompt} if self.path.startswith("/ollama") else {
                "choices": [{"message": {"content": "" if fault == "empty_completion" else prompt}}]}
        encoded = json.dumps(response, ensure_ascii=False).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


@unittest.skipUnless(os.environ.get("MYSQL"), "set MYSQL to a local test database client command")
class AIFunctionSQLTest(unittest.TestCase):
    def sql(self, query, succeeds=True):
        completed = subprocess.run(
            shlex.split(os.environ["MYSQL"]) + ["--batch", "--skip-column-names", "--raw",
                                                 "--default-character-set=utf8mb4", "-e", query],
            capture_output=True, text=True, encoding="utf-8", timeout=120)
        if succeeds:
            self.assertEqual(0, completed.returncode, completed.stderr)
        else:
            self.assertNotEqual(0, completed.returncode, completed.stdout)
        return completed.stdout.rstrip("\n")

    def setUp(self):
        self.prefix = "ai_regression_" + uuid.uuid4().hex[:12]
        self.endpoints = []
        self.models = []
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        with MockHandler.lock:
            MockHandler.requests = []
            MockHandler.fault = None

    def model(self, kind, path, provider="openai"):
        name = self.prefix + "_" + str(len(self.models))
        endpoint = name + "_endpoint"
        self.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(" + literal(name) + "," +
                 literal(json.dumps({"type": kind, "model_name": name})) + ")")
        self.models.append(name)
        self.addCleanup(self.sql, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL(" + literal(name) + ")")
        params = {"ai_model_name": name, "provider": provider,
                  "url": "http://127.0.0.1:" + str(self.server.server_port) + path,
                  "access_key": "" if provider.lower() == "ollama" else "test-only-key"}
        self.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(" + literal(endpoint) + "," +
                 literal(json.dumps(params)) + ")")
        self.endpoints.append(endpoint)
        self.addCleanup(self.sql, "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(" + literal(endpoint) + ")")
        return literal(name)

    def completion(self, model, prompt, config="NULL"):
        result = self.sql("SELECT AI_COMPLETE(" + model + "," + prompt + "," + config + ")")
        return result

    def test_prompt_json_nested_repetition_and_longtext(self):
        model = self.model("completion", "/complete")
        prompt = "AI_PROMPT('{0}/{0}: {1}', AI_PROMPT('Hi {0}', '世界'), JSON_ARRAY(1, TRUE, NULL))"
        self.assertEqual("Hi 世界/Hi 世界: [1, true, null]", self.completion(model, prompt))
        self.assertEqual("x" * 150000, self.completion(model, "AI_PROMPT('{0}{0}{0}', REPEAT('x',50000))"))
        self.sql("SELECT AI_COMPLETE(" + model + ", AI_PROMPT('{9999999999999999999999}', 'x'))", False)

    def test_completion_configuration_and_invalid_response(self):
        model = self.model("completion", "/complete")
        self.assertEqual("hello", self.completion(model, "'hello'"))
        self.completion(model, "'config'", "JSON_OBJECT('temperature',0.25,'max_tokens',123)")
        request = MockHandler.requests[-1][1]
        self.assertEqual(0.25, request["temperature"])
        self.assertEqual(123, request["max_tokens"])
        self.completion(model, "'user input'", "JSON_OBJECT('system_prompt','Be precise')")
        request = MockHandler.requests[-1][1]
        self.assertEqual([{"role": "system", "content": "Be precise"},
                          {"role": "user", "content": "user input"}], request["messages"])
        self.assertNotIn("system_prompt", request)
        with MockHandler.lock:
            MockHandler.fault = "empty_completion"
        self.sql("SELECT AI_COMPLETE(" + model + ", 'hello')", False)

    def test_ollama_native_protocol_without_access_key(self):
        model = self.model("completion", "/ollama/generate", "oLlAmA")
        self.assertEqual("local", self.completion(model, "'local'",
                         "JSON_OBJECT('temperature',0.25,'max_tokens',12)"))
        _, body, headers = MockHandler.requests[-1]
        self.assertFalse(body["stream"])
        self.assertEqual(12, body["options"]["num_predict"])
        self.assertNotIn("Authorization", headers)
        embed = self.model("dense_embedding", "/ollama/embed", "OLLAMA")
        result = self.sql("SELECT AI_EMBED(" + embed + ", 'local')")
        self.assertEqual(vector("local"), json.loads(result))

    def test_embedding_batch_order_nulls_filter_limit_and_dimension(self):
        model = self.model("dense_embedding", "/embed")
        table = "test." + self.prefix
        self.sql("CREATE TABLE " + table + "(id INT PRIMARY KEY, content LONGTEXT)")
        self.addCleanup(self.sql, "DROP TABLE " + table)
        records = [(i, None if i % 11 == 0 else "文本" + str(i)) for i in range(155)]
        self.sql("INSERT INTO " + table + " VALUES " + ",".join(
            "(" + str(i) + "," + ("NULL" if text is None else literal(text)) + ")" for i, text in records))
        output = self.sql("SELECT id, AI_EMBED(" + model + ",content,6) FROM " + table +
                          " WHERE id >= 3 ORDER BY id LIMIT 140")
        for line, (expected_id, text) in zip(output.splitlines(), records[3:143]):
            identifier, encoded = line.split("\t")
            self.assertEqual(expected_id, int(identifier))
            self.assertEqual(None if text is None else vector(text, 6),
                             None if encoded == "NULL" else json.loads(encoded))
        self.assertEqual(140, len(output.splitlines()))
        batches = [body["input"] for path, body, _ in MockHandler.requests if path == "/embed"]
        self.assertTrue(any(len(batch) > 1 for batch in batches), "SQL execution did not batch rows")
        self.assertTrue(all(all(value is not None for value in batch) for batch in batches))
        self.sql("SELECT AI_EMBED(" + model + ",'x',0)", False)
        self.sql("SELECT AI_EMBED(" + model + ",'x',1.5)", False)
        with MockHandler.lock:
            MockHandler.fault = "wrong_dimension"
        self.sql("SELECT AI_EMBED(" + model + ",'x',4)", False)

    def test_rerank_objects_multibatch_stable_ids(self):
        model = self.model("rerank", "/rerank", "siliconflow")
        documents = [{"id": "id-" + str(i), "body": "a" * (i % 7 + 1)} for i in range(47)]
        expected = sorted(documents, key=lambda doc: -(len(doc["body"]) % 3))
        result = json.loads(self.sql("SELECT AI_RERANK(" + model + ",'query'," +
                                     literal(json.dumps(documents)) + ",'body')"))
        self.assertEqual([doc["id"] for doc in expected], [doc["id"] for doc in result])
        self.assertEqual([len(doc["body"]) % 3 for doc in expected], [doc["_model_score"] for doc in result])
        sizes = [len(body["documents"]) for path, body, _ in MockHandler.requests if path == "/rerank"]
        self.assertEqual([20, 20, 7], sizes)


if __name__ == "__main__":
    unittest.main()
