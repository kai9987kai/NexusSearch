"""Low-level ctypes bindings to libnexus."""
from __future__ import annotations

import ctypes
from ctypes import (
    POINTER,
    Structure,
    Union,
    c_bool,
    c_char,
    c_char_p,
    c_double,
    c_float,
    c_int,
    c_int64,
    c_size_t,
    c_uint8,
    c_uint32,
    c_uint64,
    c_void_p,
)
import os
from pathlib import Path
import sys


def _find_library() -> ctypes.CDLL:
    custom_path = os.environ.get("NEXUS_LIB_PATH")
    candidates = []
    if custom_path:
        candidates.append(Path(custom_path))

    root = Path(__file__).resolve().parents[3]
    lib_name = "libnexus.dll" if os.name == "nt" else ("libnexus.dylib" if sys.platform == "darwin" else "libnexus.so")

    if not custom_path:
        candidates.extend([
            Path(__file__).parent / lib_name,
            root / "libnexus.dll" if os.name == "nt" else root / lib_name,
            root / "build" / lib_name,
            root / "build-release" / lib_name,
        ])

    failures = []
    for cand in candidates:
        if cand.is_file():
            try:
                # Python 3.8+ resolves dependent DLLs using explicit search
                # directories. Keep each handle alive with its loaded library;
                # never change the caller's PATH or depend on a developer SDK.
                directory = None
                if os.name == "nt" and hasattr(os, "add_dll_directory"):
                    directory = os.add_dll_directory(str(cand.resolve().parent))
                try:
                    library = ctypes.CDLL(str(cand.resolve()))
                except Exception:
                    if directory is not None:
                        directory.close()
                    raise
                library._nexus_dll_directory = directory
                return library
            except Exception as error:
                failures.append(f"{cand}: {error}")

    if custom_path:
        raise RuntimeError(
            f"Could not load explicitly selected NexusSearch library {custom_path}: "
            + ("; ".join(failures) or "file does not exist")
        )

    try:
        return ctypes.CDLL(lib_name)
    except Exception as e:
        raise RuntimeError(
            f"Could not load NexusSearch shared library ({lib_name}). "
            f"Searched: {[str(c) for c in candidates]}. "
            f"Load failures: {failures}. Error: {e}"
        )


lib = _find_library()


class nx_slice(Structure):
    _fields_ = [
        ("p", POINTER(c_uint8)),
        ("n", c_size_t),
    ]

    @classmethod
    def from_bytes(cls, b: bytes) -> tuple[nx_slice, bytes]:
        sl = cls()
        sl.n = len(b)
        sl.p = ctypes.cast(b, POINTER(c_uint8))
        return sl, b

    def to_bytes(self) -> bytes:
        if not self.p or self.n == 0:
            return b""
        return ctypes.string_at(self.p, self.n)


class nx_buf(Structure):
    _fields_ = [
        ("data", POINTER(c_uint8)),
        ("len", c_size_t),
        ("cap", c_size_t),
        ("oom", c_bool),
    ]

    def to_bytes(self) -> bytes:
        if not self.data or self.len == 0:
            return b""
        return ctypes.string_at(self.data, self.len)


class nx_error(Structure):
    _fields_ = [
        ("code", c_int),
        ("pos", c_int64),
        ("len", c_size_t),
        ("msg", c_char * 256),
    ]

    def message(self) -> str:
        return self.msg.decode("utf-8", errors="replace")


class nx_table_limits(Structure):
    _fields_ = [
        ("max_input_bytes", c_size_t),
        ("max_rows", c_size_t),
        ("max_fields", c_size_t),
        ("max_field_bytes", c_size_t),
        ("max_vector_dims", c_size_t),
    ]


class nx_table(Structure):
    _fields_ = [
        ("bytes", nx_slice),
        ("rows", c_uint32),
        ("fields", c_uint32),
    ]


class nx_field(Structure):
    _fields_ = [
        ("name", nx_slice),
        ("type", c_int),
        ("dims", c_uint32),
    ]


class _nx_vector_as(Structure):
    _fields_ = [
        ("bytes", nx_slice),
        ("dims", c_uint32),
    ]


class _nx_cell_as(Union):
    _fields_ = [
        ("integer", c_int64),
        ("real", c_double),
        ("boolean", c_bool),
        ("text", nx_slice),
        ("vector", _nx_vector_as),
    ]


class nx_cell(Structure):
    _fields_ = [
        ("present", c_bool),
        ("type", c_int),
        ("as_", _nx_cell_as),
    ]


class nx_search_options(Structure):
    _fields_ = [
        ("indexed", c_bool),
        ("max_work", c_size_t),
        ("max_hits", c_size_t),
        ("bm25_k1", c_double),
        ("bm25_b", c_double),
    ]


class nx_search_hit(Structure):
    _fields_ = [
        ("row", c_uint32),
        ("score", c_double),
        ("id", nx_slice),
        ("document", nx_slice),
    ]


class nx_search_result(Structure):
    _fields_ = [
        ("hits", POINTER(nx_search_hit)),
        ("count", c_size_t),
        ("total", c_size_t),
        ("work", c_size_t),
        ("numeric_indexes", c_size_t),
        ("scanned_cells", c_size_t),
        ("vectors_scored", c_size_t),
        ("explain_only", c_bool),
        ("indexed", c_bool),
        ("has_lexical", c_bool),
        ("has_vector", c_bool),
        ("prepared_text", c_bool),
        ("scanned_text", c_bool),
    ]


# Function prototypes
lib.nx_status_str.restype = c_char_p
lib.nx_status_str.argtypes = [c_int]

lib.nx_table_default_limits.restype = nx_table_limits
lib.nx_table_default_limits.argtypes = []

lib.nx_table_build.restype = c_int
lib.nx_table_build.argtypes = [nx_slice, POINTER(nx_table_limits), POINTER(nx_buf), POINTER(nx_error)]

lib.nx_table_open.restype = c_int
lib.nx_table_open.argtypes = [nx_slice, POINTER(nx_table), POINTER(nx_error)]

lib.nx_table_field.restype = c_int
lib.nx_table_field.argtypes = [POINTER(nx_table), c_uint32, POINTER(nx_field)]

lib.nx_table_find.restype = c_int
lib.nx_table_find.argtypes = [POINTER(nx_table), nx_slice, POINTER(c_uint32)]

lib.nx_table_get.restype = c_int
lib.nx_table_get.argtypes = [POINTER(nx_table), c_uint32, c_uint32, POINTER(nx_cell)]

lib.nx_table_id.restype = nx_slice
lib.nx_table_id.argtypes = [POINTER(nx_table), c_uint32]

lib.nx_table_document.restype = nx_slice
lib.nx_table_document.argtypes = [POINTER(nx_table), c_uint32]

lib.nx_cell_vector_at.restype = c_float
lib.nx_cell_vector_at.argtypes = [POINTER(nx_cell), c_uint32]

lib.nx_search_default_options.restype = nx_search_options
lib.nx_search_default_options.argtypes = []

lib.nx_search.restype = c_int
lib.nx_search.argtypes = [
    POINTER(nx_table),
    nx_slice,
    POINTER(nx_search_options),
    POINTER(nx_search_result),
    POINTER(nx_error),
]

lib.nx_search_index_build.restype = c_int
lib.nx_search_index_build.argtypes = [POINTER(nx_table), POINTER(c_void_p), POINTER(nx_error)]

lib.nx_search_index_free.restype = None
lib.nx_search_index_free.argtypes = [c_void_p]

lib.nx_search_index_memory_bytes.restype = c_size_t
lib.nx_search_index_memory_bytes.argtypes = [c_void_p]

lib.nx_search_index_search.restype = c_int
lib.nx_search_index_search.argtypes = [
    c_void_p,
    nx_slice,
    POINTER(nx_search_options),
    POINTER(nx_search_result),
    POINTER(nx_error),
]

lib.nx_search_result_free.restype = None
lib.nx_search_result_free.argtypes = [POINTER(nx_search_result)]

lib.nx_search_json.restype = c_int
lib.nx_search_json.argtypes = [POINTER(nx_search_result), POINTER(nx_buf)]

lib.nx_buf_init.restype = None
lib.nx_buf_init.argtypes = [POINTER(nx_buf)]

lib.nx_buf_free.restype = None
lib.nx_buf_free.argtypes = [POINTER(nx_buf)]

lib.nx_server_stats_json.restype = c_int
lib.nx_server_stats_json.argtypes = [POINTER(nx_table), POINTER(nx_buf)]

lib.nx_server_query_json.restype = c_int
lib.nx_server_query_json.argtypes = [POINTER(nx_table), c_char_p, c_bool, c_bool, POINTER(nx_buf)]
