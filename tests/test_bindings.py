"""Unit tests for NexusSearch Python SDK."""
from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

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

    def test_execution_metadata(self):
        with nexus.Snapshot.open(self.example_nxs) as snap:
            cases = (
                ("year:>=2025", False, False),
                ("title:search", True, False),
                ("embedding:[1,0,0]", False, True),
                ("title:search AND embedding:[1,0,0]", True, True),
            )
            for query, lexical, vector in cases:
                for scan in (False, True):
                    with self.subTest(query=query, scan=scan):
                        result = snap.search(query, scan=scan)
                        self.assertEqual(result.indexed, not scan)
                        self.assertEqual(result.has_lexical, lexical)
                        self.assertEqual(result.has_vector, vector)
            result = snap.search("EXPLAIN title:search")
            self.assertTrue(result.explain_only)
            self.assertTrue(result.has_lexical)
            self.assertEqual(result.work, 0)

    def test_build_replaces_only_after_complete_flush(self):
        old_source = b'{"_id":"old","text":"existing snapshot"}\n'
        new_source = b'{"_id":"new","text":"replacement snapshot"}\n'
        old_bytes = nexus.Snapshot.build(old_source)
        expected_bytes = nexus.Snapshot.build(new_source)
        real_replace = os.replace
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "snapshot Caf\u00e9.nxs"
            destination.write_bytes(old_bytes)
            unrelated = Path(directory) / ".unrelated.tmp"
            unrelated.write_bytes(b"preserve")

            def checked_replace(source, target):
                self.assertEqual(Path(source).parent, destination.parent)
                self.assertNotEqual(Path(source), destination)
                self.assertEqual(Path(source).read_bytes(), expected_bytes)
                self.assertEqual(Path(target).read_bytes(), old_bytes)
                sync.assert_called_once()
                return real_replace(source, target)

            with mock.patch("nexus.snapshot.os.fsync", wraps=os.fsync) as sync:
                with mock.patch("nexus.snapshot.os.replace", side_effect=checked_replace) as replace:
                    result = nexus.Snapshot.build(new_source, destination)
                    replace.assert_called_once()
            self.assertEqual(result, expected_bytes)
            self.assertEqual(destination.read_bytes(), expected_bytes)
            self.assertEqual(nexus.Snapshot.open(destination).get_document(0)["_id"], "new")
            self.assertEqual(unrelated.read_bytes(), b"preserve")
            self.assertEqual(set(Path(directory).iterdir()), {destination, unrelated})

    def test_failed_save_preserves_previous_snapshot_and_cleans_own_temp(self):
        old_source = b'{"_id":"old","text":"existing snapshot"}\n'
        new_source = b'{"_id":"new","text":"replacement snapshot"}\n'
        old_bytes = nexus.Snapshot.build(old_source)
        for failure_point in ("fsync", "replace"):
            for existing in (False, True):
                with self.subTest(failure_point=failure_point, existing=existing):
                    with tempfile.TemporaryDirectory() as directory:
                        destination = Path(directory) / "snapshot.nxs"
                        unrelated = Path(directory) / ".snapshot.nxs.unrelated.tmp"
                        unrelated.write_bytes(b"preserve")
                        if existing:
                            destination.write_bytes(old_bytes)
                        with mock.patch(
                            "nexus.snapshot.os." + failure_point,
                            side_effect=OSError("injected save failure"),
                        ):
                            with self.assertRaisesRegex(OSError, "injected save failure"):
                                nexus.Snapshot.build(new_source, destination)
                        if existing:
                            self.assertEqual(destination.read_bytes(), old_bytes)
                            self.assertEqual(nexus.Snapshot.open(destination).get_document(0)["_id"], "old")
                        else:
                            self.assertFalse(destination.exists())
                        expected = {unrelated, destination} if existing else {unrelated}
                        self.assertEqual(set(Path(directory).iterdir()), expected)
                        self.assertEqual(unrelated.read_bytes(), b"preserve")

    def test_build_creates_snapshot_in_new_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "new" / "nested" / "snapshot.nxs"
            expected = nexus.Snapshot.build(b'{"_id":"new","value":42}\n', destination)
            self.assertEqual(destination.read_bytes(), expected)
            self.assertEqual(nexus.Snapshot.open(destination).get_document(0)["value"], 42)
            self.assertEqual(list(destination.parent.iterdir()), [destination])


if __name__ == "__main__":
    unittest.main()
