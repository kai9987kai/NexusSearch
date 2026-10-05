"""NexusSearch Python SDK.

High-performance, dependency-free embedded search engine in C11.
"""
from .snapshot import FieldInfo, Hit, NexusError, PreparedSnapshot, SearchResult, Snapshot

__version__ = "0.3.0"
__all__ = ["Snapshot", "PreparedSnapshot", "SearchResult", "Hit", "FieldInfo", "NexusError", "__version__"]
