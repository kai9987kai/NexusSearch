"""NexusSearch Python SDK.

High-performance, dependency-free embedded search engine in C11.
"""
from .snapshot import FieldInfo, Hit, NexusError, SearchResult, Snapshot

__version__ = "0.2.0"
__all__ = ["Snapshot", "SearchResult", "Hit", "FieldInfo", "NexusError", "__version__"]
