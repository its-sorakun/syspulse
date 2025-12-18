# Implementation details:
# Thread-safe circular ring buffer for telemetry time series.
# Stores rolling snapshots over a 60-second sliding window at 1-second cadence.
# Formats historical vectors for rendering 60 FPS HTML5 Canvas trendlines.

from collections import deque
import threading
from typing import Dict, Any, List


class HistoryBuffer:
    def __init__(self, max_length: int = 60):
        self._max_length = max_length
        self._buffer: deque = deque(maxlen=max_length)
        self._lock = threading.Lock()

    def append(self, snapshot: Dict[str, Any]) -> None:
        with self._lock:
            ram_pct = 0.0
            if snapshot.get("ram_total_bytes", 0) > 0:
                ram_pct = round((snapshot["ram_used_bytes"] / snapshot["ram_total_bytes"]) * 100.0, 1)

            entry = {
                "t": snapshot.get("timestamp_ms", 0),
                "cpu": snapshot.get("cpu_usage", 0.0),
                "cpu_temp": snapshot.get("cpu_temp"),
                "gpu": snapshot.get("gpu_usage", 0.0),
                "gpu_temp": snapshot.get("gpu_temp"),
                "ram": ram_pct,
            }
            self._buffer.append(entry)

    def get_history(self) -> List[Dict[str, Any]]:
        with self._lock:
            return list(self._buffer)
