"""Black-box CLI checks; standard library only, all files in a temporary folder."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / ("nexus.exe" if os.name == "nt" else "nexus")


class CliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nexus-cli-")
        cls.folder = Path(cls.temp.name) / "données_日本"
        cls.folder.mkdir()
        cls.source = cls.folder / "documents.jsonl"
        cls.source.write_bytes((ROOT / "examples" / "documents.jsonl").read_bytes())
        cls.snapshot = cls.folder / "snapshot.nxs"
        proc = cls.invoke("build", cls.source, cls.snapshot)
        if proc.returncode:
            cls.temp.cleanup()
            raise AssertionError(f"Initial CLI build failed: {proc.stderr}")
        cls.build_report = json.loads(proc.stdout)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @staticmethod
    def invoke(*args):
        return subprocess.run(
            [str(EXE), *(str(arg) for arg in args)],
            capture_output=True,
            encoding="utf-8",
            errors="strict",
            timeout=30,
            check=False,
        )

    def command(self, *args):
        proc = self.invoke(*args)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stderr, "")
        return json.loads(proc.stdout)

    def search(self, query, scan=False):
        args = ["search", self.snapshot, query]
        if scan:
            args.append("--scan")
        return self.command(*args)

    def ids(self, result):
        return [hit["_id"] for hit in result["hits"]]

    def test_build_schema_and_unicode_paths(self):
        self.assertEqual(self.build_report["rows"], 6)
        self.assertEqual(self.build_report["bytes"], self.snapshot.stat().st_size)
        stats = self.command("stats", self.snapshot)
        self.assertEqual(stats["rows"], 6)
        fields = {field["name"]: field for field in stats["fields"]}
        self.assertEqual(fields["year"]["type"], "int")
        self.assertEqual(fields["rating"]["type"], "float")
        self.assertEqual(fields["active"]["type"], "bool")
        self.assertEqual(fields["embedding"]["dimensions"], 3)

    def test_typed_filters_ranges_and_missing(self):
        result = self.search("active:true AND year:>=2025")
        self.assertEqual(self.ids(result), ["paper-01", "paper-02", "paper-05"])
        self.assertEqual(result["total"], 3)
        self.assertEqual(self.ids(self.search("year:2024..2025")), ["paper-02", "paper-03"])
        self.assertEqual(self.ids(self.search("NOT year:*")), ["paper-06"])
        self.assertEqual(self.ids(self.search("rating:>4.6")), ["paper-01", "paper-05"])

    def test_exact_text_phrase_and_term_ranking(self):
        self.assertEqual(self.ids(self.search('title="Local search in C"')), ["paper-01"])
        self.assertEqual(self.ids(self.search('title="local search in c"')), [])
        self.assertEqual(self.ids(self.search('body:"document search"')), ["paper-02"])
        hits = self.search("title:search")["hits"]
        self.assertEqual(hits[0]["_id"], "paper-04")
        self.assertTrue(all(math.isfinite(hit["score"]) and hit["score"] > 0 for hit in hits))
        self.assertEqual({hit["_id"] for hit in hits}, {"paper-01", "paper-02", "paper-04"})

    def test_text_helpers_and_unicode(self):
        expected = {"paper-01", "paper-02", "paper-04"}
        self.assertEqual(set(self.ids(self.search("title:prefix(sea)"))), expected)
        self.assertEqual(set(self.ids(self.search("title:fuzzy(serch,1)"))), expected)
        self.assertEqual(self.ids(self.search('body:substr("immutable")')), ["paper-03"])
        self.assertEqual(self.ids(self.search("body:/snapshots?/")), ["paper-03"])
        self.assertEqual(self.ids(self.search('body:"Café"')), ["paper-05"])

    def test_exact_vector_and_hybrid(self):
        result = self.search("embedding:[1,0,0] AND active:true LIMIT 3")
        self.assertEqual(self.ids(result), ["paper-01", "paper-02", "paper-04"])
        self.assertAlmostEqual(result["hits"][0]["score"], 1.0, places=12)
        query = "title:search AND embedding:[1,0,0] LIMIT 3"
        hybrid = self.search(query)
        self.assertEqual(set(self.ids(hybrid)), {"paper-01", "paper-02", "paper-04"})
        self.assertTrue(all(0 < hit["score"] < 1 for hit in hybrid["hits"]))
        self.assertEqual(hybrid["hits"], self.search(query)["hits"])

    def test_scan_parity(self):
        for query in (
            "year:>=2025 AND active:true",
            "year:2024..2026 OR NOT active:true",
            "title:search AND year:>=2023",
            "title:search AND embedding:[1,0,0] AND year:>=2023",
            "NOT year:* OR rating:>=4.5 SORT BY year DESC, _id ASC",
        ):
            with self.subTest(query=query):
                indexed = self.search(query)
                scanned = self.search(query, scan=True)
                self.assertEqual(indexed["total"], scanned["total"])
                self.assertEqual(indexed["hits"], scanned["hits"])

    def test_sort_pagination_and_explain(self):
        result = self.search("year:* SORT BY year DESC, _id ASC LIMIT 2 OFFSET 1")
        self.assertEqual(self.ids(result), ["paper-05", "paper-02"])
        self.assertEqual(result["total"], 5)
        explain = self.command("explain", self.snapshot, "year:>=2025")
        self.assertEqual(explain["hits"], [])
        self.assertEqual(explain["total"], 0)
        analyzed = self.search("EXPLAIN ANALYZE year:>=2025")
        self.assertEqual(self.ids(analyzed), ["paper-01", "paper-02", "paper-05"])

    def test_int64_extremes_and_json_escaping(self):
        source = self.folder / "extremes.jsonl"
        target = self.folder / "extremes.nxs"
        source.write_text(
            '\n'.join(json.dumps(row, ensure_ascii=False) for row in (
                {"_id": 'quote"\\\n', "counter": 9223372036854775807, "title": "Café"},
                {"_id": "minimum", "counter": -9223372036854775808, "title": "Other"},
            )) + '\n',
            encoding="utf-8",
        )
        self.command("build", source, target)
        result = self.command("search", target, "counter=9223372036854775807")
        self.assertEqual(self.ids(result), ['quote"\\\n'])
        result = self.command("search", target, "counter=-9223372036854775808")
        self.assertEqual(self.ids(result), ["minimum"])

    def test_bad_ingestion_preserves_output(self):
        target = self.folder / "preserved.nxs"
        original = self.snapshot.read_bytes()
        target.write_bytes(original)
        source = self.folder / "bad.jsonl"
        for raw in (
            '{"_id":"same"}\n{"_id":"same"}\n',
            '{"_id":"broken",}\n',
            '{"_id":"a","v":[1,0]}\n{"_id":"b","v":[1,0,0]}\n',
            '{"_id":"a","n":1}\n{"_id":"b","n":"text"}\n',
        ):
            with self.subTest(raw=raw):
                source.write_text(raw, encoding="utf-8")
                proc = self.invoke("build", source, target)
                self.assertNotEqual(proc.returncode, 0)
                self.assertEqual(proc.stdout, "")
                self.assertIn("nexus:", proc.stderr)
                self.assertEqual(target.read_bytes(), original)

    def test_invalid_and_unsupported_queries(self):
        for query in (
            "year:(", "unknown:123", "WATCH active:true", "SOURCE(local) active:true",
            "active:true FACET year", "embedding:[1,0]", "year:1MB",
            "year:2026-01-01", "embedding:\"infer this\"",
        ):
            with self.subTest(query=query):
                proc = self.invoke("search", self.snapshot, query)
                self.assertNotEqual(proc.returncode, 0)
                self.assertEqual(proc.stdout, "")
                self.assertIn("nexus:", proc.stderr)

    def test_corruption_and_truncation_fail_before_output(self):
        original = self.snapshot.read_bytes()
        corrupt = bytearray(original)
        corrupt[len(corrupt) // 2] ^= 0x40
        for name, raw in (("corrupt.nxs", corrupt), ("short.nxs", original[:20])):
            target = self.folder / name
            target.write_bytes(raw)
            proc = self.invoke("search", target, "active:true")
            self.assertNotEqual(proc.returncode, 0)
            self.assertEqual(proc.stdout, "")
            self.assertIn("nexus:", proc.stderr)

    def test_help_version_and_usage(self):
        proc = self.invoke("--help")
        self.assertEqual(proc.returncode, 0)
        self.assertIn("nexus build", proc.stdout)
        proc = self.invoke("--version")
        self.assertEqual(proc.returncode, 0)
        self.assertIn("0.2.0", proc.stdout)
        proc = self.invoke("search", self.snapshot, "search", "--unknown")
        self.assertEqual(proc.returncode, 2)
        self.assertEqual(proc.stdout, "")
        self.assertIn("Usage:", proc.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default=os.environ.get("NEXUS_EXE", str(EXE)))
    arguments, remaining = parser.parse_known_args()
    EXE = Path(arguments.exe).resolve()
    if not EXE.is_file():
        parser.error(f"CLI executable not found: {EXE}; build NexusSearch first")
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
