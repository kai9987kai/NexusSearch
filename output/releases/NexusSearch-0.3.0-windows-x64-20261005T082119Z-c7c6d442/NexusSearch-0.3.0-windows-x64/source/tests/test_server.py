"""Integration tests for NexusSearch HTTP REST server and Web UI."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / ("nexus.exe" if os.name == "nt" else "nexus")


def get_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ServerTests(unittest.TestCase):
    proc = None
    port = 0
    temp_dir = None
    snapshot_path = None

    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory(prefix="nexus-server-test-")
        folder = Path(cls.temp_dir.name)
        cls.snapshot_path = folder / "test.nxs"

        # 1. Build test snapshot
        src_jsonl = ROOT / "examples" / "documents.jsonl"
        build_cmd = [str(EXE), "build", str(src_jsonl), str(cls.snapshot_path)]
        res = subprocess.run(build_cmd, capture_output=True, text=True, timeout=15)
        if res.returncode != 0:
            cls.temp_dir.cleanup()
            raise RuntimeError(f"Snapshot build failed: {res.stderr}")

        # 2. Pick free port and start server
        cls.port = get_free_port()
        serve_cmd = [str(EXE), "serve", str(cls.snapshot_path), "--port", str(cls.port)]
        cls.proc = subprocess.Popen(
            serve_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        # 3. Wait for server to become ready
        ready = False
        health_url = f"http://127.0.0.1:{cls.port}/health"
        start_time = time.time()
        while time.time() - start_time < 10.0:
            if cls.proc.poll() is not None:
                _, stderr = cls.proc.communicate()
                cls.temp_dir.cleanup()
                raise RuntimeError(f"Server exited prematurely: {stderr}")
            try:
                with urllib.request.urlopen(health_url, timeout=1.0) as resp:
                    if resp.status == 200:
                        ready = True
                        break
            except Exception:
                time.sleep(0.1)

        if not ready:
            cls.proc.terminate()
            cls.temp_dir.cleanup()
            raise RuntimeError(f"Server did not start within 10s on port {cls.port}")

    @classmethod
    def tearDownClass(cls):
        if cls.proc and cls.proc.poll() is None:
            cls.proc.terminate()
            try:
                cls.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                cls.proc.kill()
        if cls.temp_dir:
            cls.temp_dir.cleanup()

    def get(self, path: str) -> tuple[int, dict | str, dict]:
        url = f"http://127.0.0.1:{self.port}{path}"
        req = urllib.request.Request(url)
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                status = resp.status
                headers = dict(resp.headers)
                body = resp.read().decode("utf-8")
                try:
                    data = json.loads(body)
                except Exception:
                    data = body
                return status, data, headers
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8")
            try:
                data = json.loads(body)
            except Exception:
                data = body
            return e.code, data, dict(e.headers)

    def post_json(self, path: str, payload: dict) -> tuple[int, dict, dict]:
        url = f"http://127.0.0.1:{self.port}{path}"
        data_bytes = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=data_bytes,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=5) as resp:
                status = resp.status
                headers = dict(resp.headers)
                data = json.loads(resp.read().decode("utf-8"))
                return status, data, headers
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read().decode("utf-8")), dict(e.headers)

    def test_health(self):
        status, data, _ = self.get("/health")
        self.assertEqual(status, 200)
        self.assertEqual(data["status"], "ok")
        self.assertEqual(data["version"], "0.3.0")

    def test_stats(self):
        status, data, _ = self.get("/api/stats")
        self.assertEqual(status, 200)
        self.assertEqual(data["rows"], 6)
        names = [f["name"] for f in data["fields"]]
        self.assertIn("_id", names)
        self.assertIn("title", names)
        self.assertIn("year", names)

    def test_search_get(self):
        status, data, _ = self.get("/api/search?q=search")
        self.assertEqual(status, 200)
        self.assertGreater(data["total"], 0)
        self.assertEqual(data["hits"][0]["_id"], "paper-04")
        self.assertIn("score", data["hits"][0])
        self.assertIn("row", data["hits"][0])
        self.assertEqual(data["execution"]["lexical"], "text_postings")

    def test_search_post(self):
        status, data, _ = self.post_json("/api/search", {"query": "active:true AND year:>=2025"})
        self.assertEqual(status, 200)
        hit_ids = [h["_id"] for h in data["hits"]]
        self.assertEqual(hit_ids, ["paper-01", "paper-02", "paper-05"])

    def test_post_quotes_unicode_and_invalid_values(self):
        for query, expected in [('title="Local search in C"', ["paper-01"]),
                                ('body:"Café"', ["paper-05"])]:
            status, result, _ = self.post_json("/api/search", {"query": query})
            self.assertEqual(status, 200)
            self.assertEqual([hit["_id"] for hit in result["hits"]], expected)
        for payload in ({"query": 12}, {"query": "*\u0000 OR title:cat"},
                        {"query": "*", "scan": "false"}, {"query": "a" * 3000}):
            status, result, _ = self.post_json("/api/search", payload)
            self.assertEqual(status, 400)
            self.assertIn("error", result)

    def test_url_input_never_truncates_or_decodes_nul(self):
        for query in ("%2a%00OR+bad:query", "%GG", "a" * 3000):
            status, result, _ = self.get("/api/search?q=" + query)
            self.assertEqual(status, 400)
            self.assertIn("error", result)

    def test_search_vectors(self):
        status, data, _ = self.get("/api/search?q=" + urllib.parse.quote("embedding:[1,0,0] LIMIT 3"))
        self.assertEqual(status, 200)
        self.assertEqual(len(data["hits"]), 3)
        self.assertEqual(data["hits"][0]["_id"], "paper-01")

    def test_explain(self):
        status, data, _ = self.get("/api/explain?q=" + urllib.parse.quote("year:>=2025"))
        self.assertEqual(status, 200)
        self.assertTrue(data.get("explain_only"))

    def test_doc(self):
        status, data, _ = self.get("/api/doc?row=0")
        self.assertEqual(status, 200)
        self.assertEqual(data["_id"], "paper-01")

    def test_doc_rejects_invalid_row_without_wrapping(self):
        for row in ("4294967296", "18446744073709551616", "-1", "abc", "0junk", ""):
            status, data, _ = self.get("/api/doc?row=" + row)
            self.assertEqual(status, 404)
            self.assertIn("error", data)

    def test_http_framing_and_case_insensitive_length(self):
        body = json.dumps({"query": 'title="Local search in C"'}).encode()
        headers = (b"cOnTeNt-LeNgTh: " + str(len(body)).encode(),
                   b"Content-Length: -1",
                   b"Content-Length: 99999999999999999999",
                   b"Content-Length: 0\r\nContent-Length: 0",
                   b"Transfer-Encoding: chunked")
        for index, header in enumerate(headers):
            with socket.create_connection(("127.0.0.1", self.port), timeout=5) as client:
                client.sendall(f"POST /api/search HTTP/1.1\r\nHost: localhost:{self.port}\r\n".encode() + header + b"\r\n\r\n" + body)
                chunks = []
                while chunk := client.recv(65536):
                    chunks.append(chunk)
            head, payload = b"".join(chunks).split(b"\r\n\r\n", 1)
            result = json.loads(payload)
            self.assertIn(b" 200 " if index == 0 else b" 400 ", head.split(b"\r\n", 1)[0])
            if index == 0:
                self.assertEqual(result["hits"][0]["_id"], "paper-01")
            else:
                self.assertIn("error", result)

    def test_web_ui(self):
        status, data, headers = self.get("/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers.get("Content-Type", ""))
        self.assertIn("NexusSearch", data)

    def test_cors(self):
        status, _, headers = self.get("/health")
        self.assertEqual(status, 200)
        self.assertNotIn("Access-Control-Allow-Origin", headers)

    def test_same_origin_and_rebinding_protection(self):
        url = f"http://127.0.0.1:{self.port}/health"
        allowed = urllib.request.Request(url, headers={"Origin":f"http://127.0.0.1:{self.port}"})
        with urllib.request.urlopen(allowed, timeout=5) as response:
            self.assertEqual(response.status, 200)
        for headers in ({"Origin":"https://unrelated.example"}, {"Origin":"null"},
                        {"Host":f"unrelated.example:{self.port}"}):
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(urllib.request.Request(url, headers=headers),timeout=5)
            self.assertEqual(error.exception.code,403)
            self.assertIn('error',json.loads(error.exception.read()))

    def test_bad_query(self):
        status, data, _ = self.get("/api/search?q=unknown_field:123")
        self.assertEqual(status, 200)
        self.assertIn("error", data)

    def test_stream_sse(self):
        url = f"http://127.0.0.1:{self.port}/api/stream?q=search"
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=5) as resp:
            self.assertEqual(resp.status, 200)
            self.assertIn("text/event-stream", resp.headers.get("Content-Type", ""))
            body = resp.read().decode("utf-8")
            self.assertIn("event: start", body)
            self.assertIn("event: hit", body)
            self.assertIn("event: done", body)
            self.assertIn("paper-04", body)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default=os.environ.get("NEXUS_EXE", str(EXE)))
    arguments, remaining = parser.parse_known_args()
    EXE = Path(arguments.exe).resolve()
    if not EXE.is_file():
        parser.error(f"CLI executable not found: {EXE}")
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
