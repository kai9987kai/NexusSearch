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
        self.assertEqual(data["version"], "0.2.0")

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

    def test_search_post(self):
        status, data, _ = self.post_json("/api/search", {"query": "active:true AND year:>=2025"})
        self.assertEqual(status, 200)
        hit_ids = [h["_id"] for h in data["hits"]]
        self.assertEqual(hit_ids, ["paper-01", "paper-02", "paper-05"])

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

    def test_web_ui(self):
        status, data, headers = self.get("/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", headers.get("Content-Type", ""))
        self.assertIn("NexusSearch", data)

    def test_cors(self):
        status, _, headers = self.get("/health")
        self.assertEqual(headers.get("Access-Control-Allow-Origin"), "*")

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
