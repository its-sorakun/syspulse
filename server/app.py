# Implementation details:
# Multi-threaded HTTP server providing telemetry REST APIs and static dashboard delivery.
# Built on standard library http.server to avoid third-party server dependencies.
# Orchestrates asynchronous background polling across native shared memory and GPU probes,
# maintaining a synchronized circular history buffer for client visualization.

import json
import os
import sys
import threading
import time
from http import HTTPStatus
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from typing import Dict, Any

from collector import TelemetryCollector
from gpu_probe import GpuProbe
from history_buffer import HistoryBuffer

BASE_DIR = Path(__file__).resolve().parent.parent
WEB_DIR = BASE_DIR / "web"
CONFIG_PATH = BASE_DIR / "config.json"


def load_config() -> Dict[str, Any]:
    default_config = {
        "host": "0.0.0.0",
        "port": 8080,
        "poll_interval_sec": 1.0,
        "history_window_seconds": 60,
        "thermal_thresholds": {
            "warning": 75.0,
            "critical": 85.0
        }
    }
    if CONFIG_PATH.exists():
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                user_cfg = json.load(f)
                default_config.update(user_cfg)
        except Exception:
            pass
    return default_config


config = load_config()
collector = TelemetryCollector()
gpu_probe = GpuProbe()
history_buffer = HistoryBuffer(max_length=config.get("history_window_seconds", 60))

latest_snapshot: Dict[str, Any] = {}
snapshot_lock = threading.Lock()
start_timestamp = time.time()


def background_sampler_loop() -> None:
    global latest_snapshot
    interval = config.get("poll_interval_sec", 1.0)

    while True:
        try:
            vitals = collector.collect()
            gpu_data = gpu_probe.query()

            # Merge GPU findings into vitals snapshot
            vitals["gpu_name"] = gpu_data.get("gpu_name", "N/A")
            if vitals.get("gpu_usage", 0.0) == 0.0:
                vitals["gpu_usage"] = gpu_data.get("gpu_usage", 0.0)
            if vitals.get("gpu_temp") is None:
                vitals["gpu_temp"] = gpu_data.get("gpu_temp")
            if vitals.get("vram_total_bytes", 0) == 0:
                vitals["vram_total_bytes"] = gpu_data.get("vram_total_bytes", 0)
                vitals["vram_used_bytes"] = gpu_data.get("vram_used_bytes", 0)

            history_buffer.append(vitals)

            with snapshot_lock:
                latest_snapshot = vitals

        except Exception as e:
            pass

        time.sleep(interval)


class SysPulseRequestHandler(BaseHTTPRequestHandler):
    def log_message(self, format: str, *args: Any) -> None:
        # Suppress routine request logs to keep terminal clear for telemetry events
        pass

    def send_json(self, status_code: int, data: Any) -> None:
        payload = json.dumps(data).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.end_headers()
        self.wfile.write(payload)

    def send_file(self, file_path: Path, mime_type: str) -> None:
        if not file_path.exists() or not file_path.is_file():
            self.send_error(HTTPStatus.NOT_FOUND, "Resource not found")
            return

        with open(file_path, "rb") as f:
            content = f.read()

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", mime_type)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(content)

    def do_GET(self) -> None:
        path = self.path.split("?")[0]

        if path == "/api/vitals":
            with snapshot_lock:
                data = dict(latest_snapshot)
            self.send_json(HTTPStatus.OK, data)

        elif path == "/api/history":
            history_data = history_buffer.get_history()
            self.send_json(HTTPStatus.OK, history_data)

        elif path == "/api/health":
            self.send_json(HTTPStatus.OK, {
                "status": "healthy",
                "uptime_seconds": int(time.time() - start_timestamp),
                "daemon_connected": latest_snapshot.get("source") == "native_daemon",
                "poll_interval_sec": config.get("poll_interval_sec", 1.0)
            })

        elif path in ("/", "/index.html"):
            self.send_file(WEB_DIR / "index.html", "text/html; charset=utf-8")

        elif path == "/style.css":
            self.send_file(WEB_DIR / "style.css", "text/css; charset=utf-8")

        elif path == "/app.js":
            self.send_file(WEB_DIR / "app.js", "application/javascript; charset=utf-8")

        elif path == "/gauges.js":
            self.send_file(WEB_DIR / "gauges.js", "application/javascript; charset=utf-8")

        else:
            self.send_error(HTTPStatus.NOT_FOUND, "Endpoint not found")


def run_server() -> None:
    sampler_thread = threading.Thread(target=background_sampler_loop, daemon=True)
    sampler_thread.start()

    host = config.get("host", "0.0.0.0")
    port = config.get("port", 8080)

    server = ThreadingHTTPServer((host, port), SysPulseRequestHandler)
    print(f"[SysPulse Server] Running on http://localhost:{port}")
    print(f"[SysPulse Server] Second-screen LAN dashboard: http://<LAN-IP>:{port}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[SysPulse Server] Stopping server...")
        server.shutdown()


if __name__ == "__main__":
    run_server()
