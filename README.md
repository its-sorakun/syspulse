<!--
Implementation details:
Documentation and architecture manual for the SysPulse personal hardware monitoring system.
Details the shared memory IPC contract, C++ Win32 hardware probe, Python telemetry server,
and responsive WebUI dashboard.
-->

# SysPulse ~ Personal Hardware Vitals & Telemetry Dashboard

A lightweight, low-overhead hardware monitoring system designed for personal workstations. Pairs a native C++ Win32 background collector with a zero-dependency Python REST server and a glassmorphic WebUI dashboard.

![Architecture](https://img.shields.io/badge/Architecture-C%2B%2B%20%2B%20Python%20Shared%20Memory-7dcfff)
![Interface](https://img.shields.io/badge/WebUI-HTML5%20Canvas%2060FPS-bb9af7)
![Platform](https://img.shields.io/badge/Platform-Windows%20x64-7aa2f7)

---

## Architecture Overview

```text
  [ Windows Hardware Sensors ]
               │
               ▼
   [ C++ Native Daemon ]
   (PDH / WMI / GlobalMemoryStatusEx)
               │
               ▼  Win32 Named Shared Memory (Local\SysPulse_Vitals)
   [ Python Telemetry Server ] ──► (nvidia-smi / DXGI GPU Probes)
   (HTTP REST / Circular Ring Buffer)
               │
               ▼  Local Network HTTP (port 8080)
   [ Responsive WebUI Dashboard ]
   (SVG Radial Meters + HTML5 Canvas 60 FPS Sparklines)
```

### Key Technical Mechanisms

1. **Zero-Copy Named Shared Memory IPC**:
   - The C++ native daemon allocates a 128-byte page-file-backed shared memory region tagged `Local\SysPulse_Vitals`.
   - The Python server accesses this block via the `mmap` standard library module, deserializing binary metrics using `struct.unpack` with near-zero CPU overhead.
2. **Dual-Path Hardware Telemetry**:
   - Primary: Win32 Performance Data Helper (`pdh.dll`) for CPU utilization and core metrics.
   - Secondary: Fallback to kernel system times differential (`GetSystemTimes`) when PDH query providers are unavailable.
   - Thermal telemetry: Queries named SMU memory blocks and driver query interfaces.
3. **Headless GPU Probing**:
   - Probes dedicated NVIDIA hardware via headless `nvidia-smi` queries with `CREATE_NO_WINDOW` flags to eliminate console blinking.
   - Collects core load, junction temperatures, and dedicated VRAM allocation.
4. **Second-Screen LAN Dashboard**:
   - The WebUI is served over standard HTTP (`http://<LAN-IP>:8080`), allowing smartphones or tablets on the same network to function as a live physical PC sensor panel.

---

## Project Structure

```text
SysPulse/
├── core/
│   ├── include/
│   │   ├── shared_memory.h    # 128-byte packed struct contract
│   │   └── hardware_probe.h   # PDH and Win32 probe interface
│   ├── hardware_probe.cpp     # PDH counter collection and memory queries
│   └── daemon_main.cpp        # Native daemon entrypoint and polling loop
├── server/
│   ├── collector.py           # Shared memory reader with native fallback
│   ├── gpu_probe.py           # GPU load, VRAM, and thermal junction probe
│   ├── history_buffer.py      # 60-second circular sliding window
│   └── app.py                 # Multi-threaded HTTP server and REST endpoints
├── web/
│   ├── index.html             # Semantic dashboard interface
│   ├── style.css              # Tokyo Night slate glassmorphic styles
│   ├── app.js                 # Polling coordinator and DOM updater
│   └── gauges.js              # Canvas Bézier sparklines & radial SVG gauges
├── config.json                # Server port, polling interval, and alert limits
└── README.md                  # Documentation and API reference
```

---

## API Endpoints

- `GET /` — Serves the responsive WebUI dashboard.
- `GET /api/vitals` — Returns the current real-time hardware telemetry snapshot (JSON).
- `GET /api/history` — Returns the circular 60-second sliding history array for trendlines (JSON).
- `GET /api/health` — Returns server uptime and daemon IPC connectivity status.

---

## Running SysPulse

### 1. Start the Telemetry Server

SysPulse runs out of the box with zero third-party dependencies using Python 3:

```bash
python server/app.py
```

Access the dashboard in your browser:
- Local Machine: `http://localhost:8080`
- Mobile / Second Screen: `http://<YOUR-PC-IP>:8080`

### 2. Optional: Build the C++ High-Frequency Daemon

To enable direct kernel-level PDH sampling and low-overhead shared memory:

```bash
cl.exe /O2 /EHsc /std:c++17 core/daemon_main.cpp core/hardware_probe.cpp pdh.lib user32.lib /Fe:bin/syspulse_daemon.exe
```

