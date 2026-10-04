"""High-level Pythonic interface for NexusSearch snapshots."""
from __future__ import annotations

import ctypes
from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any, Optional, Union

from . import _ffi as ffi


class NexusError(Exception):
    """Base exception for NexusSearch operations."""
    def __init__(self, status_code: int, message: str = ""):
        self.status_code = status_code
        status_name = ffi.lib.nx_status_str(status_code).decode("utf-8", errors="replace")
        full_msg = f"{status_name}: {message}" if message else status_name
        super().__init__(full_msg)


@dataclass(frozen=True)
class FieldInfo:
    name: str
    type: str
    dimensions: int = 0


@dataclass
class Hit:
    id: str
    row: int
    score: float
    raw_document: str
    _doc_cache: Optional[dict[str, Any]] = None

    @property
    def document(self) -> dict[str, Any]:
        if self._doc_cache is None:
            try:
                self._doc_cache = json.loads(self.raw_document)
            except Exception:
                self._doc_cache = {}
        return self._doc_cache


@dataclass
class SearchResult:
    hits: list[Hit]
    total: int
    count: int
    work: int
    numeric_indexes: int
    scanned_cells: int
    vectors_scored: int
    explain_only: bool

    def __iter__(self):
        return iter(self.hits)

    def __len__(self) -> int:
        return len(self.hits)

    def __getitem__(self, idx: int) -> Hit:
        return self.hits[idx]


class Snapshot:
    """An immutable, memory-mapped or in-memory NexusSearch document snapshot."""

    def __init__(self, data: bytes):
        self._data = data
        self._table = ffi.nx_table()
        sl, _ = ffi.nx_slice.from_bytes(data)
        err = ffi.nx_error()
        st = ffi.lib.nx_table_open(sl, ctypes.byref(self._table), ctypes.byref(err))
        if st != 0:
            raise NexusError(st, err.message())

    @classmethod
    def open(cls, path_or_bytes: Union[str, Path, bytes]) -> Snapshot:
        if isinstance(path_or_bytes, (str, Path)):
            with open(path_or_bytes, "rb") as f:
                data = f.read()
            return cls(data)
        elif isinstance(path_or_bytes, (bytes, bytearray)):
            return cls(bytes(path_or_bytes))
        else:
            raise TypeError("Expected filepath or bytes")

    @classmethod
    def build(
        cls,
        jsonl_source: Union[str, Path, bytes],
        output_path: Optional[Union[str, Path]] = None,
        max_rows: Optional[int] = None,
    ) -> bytes:
        if isinstance(jsonl_source, (str, Path)):
            with open(jsonl_source, "rb") as f:
                jsonl_bytes = f.read()
        else:
            jsonl_bytes = bytes(jsonl_source)

        limits = ffi.lib.nx_table_default_limits()
        if max_rows is not None:
            limits.max_rows = max_rows

        sl, _ = ffi.nx_slice.from_bytes(jsonl_bytes)
        out_buf = ffi.nx_buf()
        ffi.lib.nx_buf_init(ctypes.byref(out_buf))
        err = ffi.nx_error()

        try:
            st = ffi.lib.nx_table_build(sl, ctypes.byref(limits), ctypes.byref(out_buf), ctypes.byref(err))
            if st != 0:
                raise NexusError(st, err.message())
            result_bytes = out_buf.to_bytes()
        finally:
            ffi.lib.nx_buf_free(ctypes.byref(out_buf))

        if output_path is not None:
            p = Path(output_path)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(result_bytes)

        return result_bytes

    def __enter__(self) -> Snapshot:
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        pass

    @property
    def rows(self) -> int:
        return self._table.rows

    @property
    def fields_count(self) -> int:
        return self._table.fields

    def stats(self) -> dict[str, Any]:
        buf = ffi.nx_buf()
        ffi.lib.nx_buf_init(ctypes.byref(buf))
        try:
            st = ffi.lib.nx_server_stats_json(ctypes.byref(self._table), ctypes.byref(buf))
            if st != 0:
                raise NexusError(st)
            return json.loads(buf.to_bytes().decode("utf-8"))
        finally:
            ffi.lib.nx_buf_free(ctypes.byref(buf))

    def search(
        self,
        query: str,
        scan: bool = False,
        limit: int = 20,
    ) -> SearchResult:
        query_bytes = query.encode("utf-8")
        sl, _ = ffi.nx_slice.from_bytes(query_bytes)

        opts = ffi.lib.nx_search_default_options()
        opts.indexed = not scan
        opts.max_hits = limit

        result = ffi.nx_search_result()
        err = ffi.nx_error()

        try:
            st = ffi.lib.nx_search(
                ctypes.byref(self._table),
                sl,
                ctypes.byref(opts),
                ctypes.byref(result),
                ctypes.byref(err),
            )
            if st != 0:
                raise NexusError(st, err.message())

            hits = []
            for i in range(result.count):
                h = result.hits[i]
                doc_bytes = h.document.to_bytes()
                id_bytes = h.id.to_bytes()
                hits.append(
                    Hit(
                        id=id_bytes.decode("utf-8", errors="replace"),
                        row=h.row,
                        score=h.score,
                        raw_document=doc_bytes.decode("utf-8", errors="replace"),
                    )
                )

            return SearchResult(
                hits=hits,
                total=result.total,
                count=result.count,
                work=result.work,
                numeric_indexes=result.numeric_indexes,
                scanned_cells=result.scanned_cells,
                vectors_scored=result.vectors_scored,
                explain_only=result.explain_only,
            )
        finally:
            ffi.lib.nx_search_result_free(ctypes.byref(result))

    def explain(self, query: str) -> dict[str, Any]:
        buf = ffi.nx_buf()
        ffi.lib.nx_buf_init(ctypes.byref(buf))
        try:
            st = ffi.lib.nx_server_query_json(
                ctypes.byref(self._table),
                query.encode("utf-8"),
                False,
                True,
                ctypes.byref(buf),
            )
            if st != 0:
                raise NexusError(st)
            return json.loads(buf.to_bytes().decode("utf-8"))
        finally:
            ffi.lib.nx_buf_free(ctypes.byref(buf))

    def get_document(self, row: int) -> dict[str, Any]:
        if row < 0 or row >= self.rows:
            raise IndexError(f"Row {row} out of range [0, {self.rows})")
        sl = ffi.lib.nx_table_document(ctypes.byref(self._table), row)
        raw = sl.to_bytes().decode("utf-8", errors="replace")
        return json.loads(raw)
