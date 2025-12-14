# Implementation details:
# GPU metrics probe using native Windows management and driver utilities.
# Probes NVIDIA hardware via headless nvidia-smi execution with CREATE_NO_WINDOW flags.
# Parses utilization, core thermal junction, and VRAM allocation into telemetry dictionaries.
# Falls back gracefully when dedicated GPU hardware or vendor drivers are unavailable.

import subprocess
import shutil
from typing import Dict, Any, Optional

CREATE_NO_WINDOW = 0x08000000


class GpuProbe:
    def __init__(self):
        self._nvidia_smi_path = shutil.which("nvidia-smi")
        self._available = self._nvidia_smi_path is not None

    def query(self) -> Dict[str, Any]:
        if not self._available:
            return {
                "gpu_name": "Integrated / Generic Display",
                "gpu_usage": 0.0,
                "gpu_temp": None,
                "vram_total_bytes": 0,
                "vram_used_bytes": 0,
            }

        try:
            cmd = [
                self._nvidia_smi_path,
                "--query-gpu=name,utilization.gpu,temperature.gpu,memory.total,memory.used",
                "--format=csv,noheader,nounits"
            ]
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=1.5,
                creationflags=CREATE_NO_WINDOW
            )

            if result.returncode != 0 or not result.stdout.strip():
                return self._fallback_result()

            line = result.stdout.strip().splitlines()[0]
            parts = [p.strip() for p in line.split(",")]

            if len(parts) >= 5:
                name = parts[0]
                usage = float(parts[1]) if parts[1].replace(".", "", 1).isdigit() else 0.0
                temp = float(parts[2]) if parts[2].replace(".", "", 1).isdigit() else None
                total_mb = float(parts[3]) if parts[3].replace(".", "", 1).isdigit() else 0.0
                used_mb = float(parts[4]) if parts[4].replace(".", "", 1).isdigit() else 0.0

                return {
                    "gpu_name": name,
                    "gpu_usage": round(usage, 1),
                    "gpu_temp": round(temp, 1) if temp is not None else None,
                    "vram_total_bytes": int(total_mb * 1024 * 1024),
                    "vram_used_bytes": int(used_mb * 1024 * 1024),
                }

        except (subprocess.SubprocessError, ValueError, OSError):
            pass

        return self._fallback_result()

    def _fallback_result(self) -> Dict[str, Any]:
        return {
            "gpu_name": "GPU Driver Offline",
            "gpu_usage": 0.0,
            "gpu_temp": None,
            "vram_total_bytes": 0,
            "vram_used_bytes": 0,
        }
