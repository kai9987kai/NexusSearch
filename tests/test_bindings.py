"""Unit tests for NexusSearch Python SDK."""
from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bindings" / "python"))

import nexus  # noqa: E402


class BindingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.example_nxs = ROOT / "build" / "example.nxs"
        if not cls.example_nxs.is_file():
            # Build example snapshot
            jsonl = ROOT / "examples" / "documents.jsonl"
            nexus.Snapshot.build(jsonl, cls.example_nxs)

    def test_open_and_properties(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            self.assertEqual(snap.rows, 6)
            self.assertEqual(snap.fields_count, 7)

    def test_stats(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            st = snap.stats()
            self.assertEqual(st["version"], "0.2.0")
            self.assertEqual(st["rows"], 6)
            field_names = [f["name"] for f in st["fields"]]
            self.assertIn("_id", field_names)
            self.assertIn("title", field_names)
            self.assertIn("year", field_names)
            self.assertIn("embedding", field_names)

    def test_search_text(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            res = snap.search("search")
            self.assertGreater(len(res), 0)
            self.assertEqual(res.hits[0].id, "paper-04")
            self.assertTrue(all(h.score > 0 for h in res))
            self.assertIn("document", dir(res.hits[0]))
            self.assertIsInstance(res.hits[0].document, dict)

    def test_search_filters(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            res = snap.search("active:true AND year:>=2025")
            hit_ids = [h.id for h in res]
            self.assertEqual(hit_ids, ["paper-01", "paper-02", "paper-05"])
            self.assertEqual(res.total, 3)

    def test_search_vectors(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            res = snap.search("embedding:[1,0,0] LIMIT 3")
            self.assertEqual(len(res), 3)
            self.assertGreater(res.vectors_scored, 0)
            self.assertEqual(res.hits[0].id, "paper-01")

    def test_scan_parity(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            r_idx = snap.search("year:>=2024 AND active:true")
            r_scn = snap.search("year:>=2024 AND active:true", scan=True)
            self.assertEqual([h.id for h in r_idx], [h.id for h in r_scn])
            for h1, h2 in zip(r_idx, r_scn):
                self.assertAlmostEqual(h1.score, h2.score, places=12)

    def test_explain(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            exp = snap.explain("year:>=2025 AND active:true")
            self.assertTrue(exp.get("explain_only"))

    def test_get_document(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            doc = snap.get_document(0)
            self.assertIsInstance(doc, dict)
            self.assertEqual(doc["_id"], "paper-01")
            with self.assertRaises(IndexError):
                snap.get_document(100)

    def test_build_in_memory_and_query(self):
        sample_jsonl = (
            b'{"_id":"x1","text":"quantum computing","val":42}\n'
            b'{"_id":"x2","text":"classical physics","val":100}\n'
        )
        snap_bytes = nexus.Snapshot.build(sample_jsonl)
        self.assertGreater(len(snap_bytes), 0)

        with nexus.Snapshot.open(snap_bytes) as snap:
            self.assertEqual(snap.rows, 2)
            res = snap.search("quantum")
            self.assertEqual(len(res), 1)
            self.assertEqual(res.hits[0].id, "x1")

    def test_bad_query_raises_error(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            with self.assertRaises(nexus.NexusError):
                snap.search("nonexistent_field:foo")


if __name__ == "__main__":
    unittest.main()
