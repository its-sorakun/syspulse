<!--
Implementation details:
Documentation and architecture manual for the SysPulse personal hardware monitoring system.
Details the v2 shared memory IPC contract, C++ Win32 hardware probe, Python telemetry server,
and responsive WebUI dashboard. Covers per-core telemetry, disk/network I/O counters,
drive capacities, power status, top-N process tracking, and WMI ACPI thermal querying.
-->

# SysPulse ~ Personal Hardware Vitals & Telemetry Dashboard

A lightweight, low-overhead hardware monitoring system designed for personal workstations. Pairs a native C++ Win32 background collector with a zero-dependency Python REST server and a glassmorphic WebUI dashboard.

![Architecture](https://img.shields.io/badge/Architecture-C%2B%2B%20%2B%20Python%20Shared%20Memory-7dcfff)
![Interface](https://img.shields.io/badge/WebUI-HTML5%20Canvas%2060FPS-bb9af7)
![Platform](https://img.shields.io/badge/Platform-Windows%20x64-7aa2f7)
![Version](https://img.shields.io/badge/Schema-v2%20(1536%20bytes)-9ece6a)

---

## Architecture Overview

```text
  [ Windows Hardware Subsystems ]
  (PDH Counters / CallNtPowerInformation / WMI ACPI / Toolhelp32)
               │
               ▼
   [ C++ Native Collector Daemon ]
   (1-second sampling cadence, zero-allocation ring queries)
               │
               ▼  Win32 Named Shared Memory (Local\SysPulse_Vitals, 1536 bytes)
   [ Python Telemetry Server ] ──► (nvidia-smi / Headless GPU Probes)
   (HTTP REST / Circular Sliding Window Buffer)
               │
               ▼  Local Network HTTP (port 8080)
   [ Responsive WebUI Dashboard ]
   (SVG Radial Meters + HTML5 Canvas 60 FPS Sparklines)
```

### Key Technical Mechanisms

1. **Zero-Copy Named Shared Memory IPC (Schema v2)**:
   - The C++ native daemon allocates a 1536-byte page-file-backed shared memory region tagged `Local\SysPulse_Vitals`.
   - Uses strict 8-byte packing alignment (`#pragma pack(push, 8)`) verified by compile-time `static_assert(sizeof(SysPulseVitals) == 1536)` to ensure layout identity between MSVC/MinGW x64 binaries and Python struct readers.
   - Guarded by magic header validation (`0x53595350` / `"SYSP"`) and schema version checks.

2. **Multi-Subsystem C++ Hardware Telemetry**:
   - **Per-Core CPU Utilization**: Performance Data Helper (`pdh.dll`) querying `\Processor(N)\% Processor Time` for up to 32 logical cores alongside aggregate total load. Fallback to kernel system times differential (`GetSystemTimes`).
   - **Real Dynamic Core Frequency**: Direct invocation of kernel power information via `CallNtPowerInformation(ProcessorInformation)` from `powrprof.dll`, extracting `CurrentMhz` across cores instead of static base clock metadata.
   - **Disk & Network Throughput**: High-frequency sampling of `\PhysicalDisk(_Total)\Disk Read Bytes/sec`, `\PhysicalDisk(_Total)\Disk Write Bytes/sec`, and interface packet byte counters.
   - **Per-Drive Volume Monitoring**: Enumerate logical drives via `GetLogicalDriveStringsW` and query cluster allocations using `GetDiskFreeSpaceExW`.
   - **Power & Battery Diagnostics**: Real-time AC connection status, charge percentages, and runtime estimates via `GetSystemPowerStatus`.
   - **WMI ACPI Thermal Zones**: COM interface querying `MSAcpi_ThermalZoneTemperature` from `root\WMI` for motherboard and package sensors (Kelvin tenths to Celsius conversion).
   - **Top-N Process Consumer Tracking**: Toolhelp32 process snapshotting paired with `GetProcessTimes` differential calculation and `GetProcessMemoryInfo` working-set queries, identifying top CPU consumers dynamically.

3. **Headless GPU Probing**:
   - Probes dedicated NVIDIA hardware via headless `nvidia-smi` queries with `CREATE_NO_WINDOW` flags to eliminate console flashing.
   - Collects core load, junction temperatures, board wattage, and dedicated VRAM allocation.

4. **Second-Screen LAN Dashboard**:
   - The WebUI is served over standard HTTP (`http://<LAN-IP>:8080`), allowing smartphones, tablets, or dedicated mini-monitors on the local network to function as a live physical sensor panel.

---

## Shared Memory Layout (v2 Contract)

The binary shared memory layout is strictly specified in `core/include/shared_memory.h`:

| Offset | Type / Size | Field | Description |
|---|---|---|---|
| `0x000` | `uint32_t` (4B) | `magic` | Magic identifier `0x53595350` (`"SYSP"`) |
| `0x004` | `uint32_t` (4B) | `version` | Schema version (`2`) |
| `0x008` | `uint64_t` (8B) | `timestamp_ms` | Millisecond Unix timestamp of sample |
| `0x010` | `double` (8B) | `cpu_usage_percent` | Aggregate CPU load percentage `[0.0 - 100.0]` |
| `0x018` | `double` (8B) | `cpu_temp_celsius` | Package temperature from ACPI thermal zone |
| `0x020` | `uint64_t` (8B) | `cpu_freq_mhz` | Active CPU frequency in MHz |
| `0x028` | `uint32_t` (4B) | `logical_core_count` | Number of detected logical processor cores |
| `0x02C` | `uint32_t` (4B) | `_pad_cpu` | Alignment padding |
| `0x030` | `double[32]` (256B) | `per_core_usage` | Per-logical-core CPU utilization array |
| `0x130` | `uint64_t` (8B) | `ram_total_bytes` | Total installed physical RAM in bytes |
| `0x138` | `uint64_t` (8B) | `ram_used_bytes` | Allocated physical RAM in bytes |
| `0x140` | `double` (8B) | `gpu_usage_percent` | 3D graphics engine utilization percentage |
| `0x148` | `double` (8B) | `gpu_temp_celsius` | GPU thermal junction temperature |
| `0x150` | `uint64_t` (8B) | `gpu_vram_total_bytes` | Total dedicated video memory |
| `0x158` | `uint64_t` (8B) | `gpu_vram_used_bytes` | Allocated dedicated video memory |
| `0x160` | `double` (8B) | `gpu_power_watts` | Board power consumption in Watts |
| `0x168` | `uint64_t` (8B) | `disk_read_bytes_sec` | Total disk read throughput in bytes/sec |
| `0x170` | `uint64_t` (8B) | `disk_write_bytes_sec` | Total disk write throughput in bytes/sec |
| `0x178` | `uint64_t` (8B) | `net_recv_bytes_sec` | Primary interface incoming bytes/sec |
| `0x180` | `uint64_t` (8B) | `net_sent_bytes_sec` | Primary interface outgoing bytes/sec |
| `0x188` | `uint32_t` (4B) | `process_count` | Total active system process count |
| `0x18C` | `uint32_t` (4B) | `uptime_seconds` | System kernel uptime in seconds |
| `0x190` | `PowerInfo` (8B) | `power` | AC flag, battery presence, charge %, seconds |
| `0x198` | `uint32_t` + `pad` (8B)| `drive_count` | Number of detected storage volumes |
| `0x1A0` | `DriveInfo[8]` (192B)| `drives` | Mount letter, total bytes, and free bytes |
| `0x260` | `uint32_t` + `pad` (8B)| `top_proc_count` | Valid entries in top-processes list |
| `0x268` | `ProcessInfo[10]` (880B)| `top_procs` | Name, PID, CPU %, memory working set |
| `0x5D8` | `uint32_t` + `pad` (8B)| `thermal_zone_count` | Valid WMI ACPI thermal readings count |
| `0x5E0` | `double[4]` (32B) | `thermal_zones` | Individual thermal sensor readings |

**Total Struct Footprint:** Exactly 1536 bytes.

---

## Project Structure

```text
SysPulse/
├── core/
│   ├── include/
│   │   ├── shared_memory.h    # 1536-byte v2 packed struct contract
│   │   └── hardware_probe.h   # Multi-subsystem probe class declarations
│   ├── hardware_probe.cpp     # PDH, WMI, NtPower, and Win32 probe engine
│   └── daemon_main.cpp        # Native daemon entrypoint and sampling loop
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
├── run_git.py                 # Repository commit timeline orchestrator
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

### 2. Building the C++ Native Telemetry Daemon

To enable direct kernel-level PDH sampling, WMI thermal readings, and low-overhead shared memory:

**MinGW-w64 (GCC):**
```bash
g++ -O2 -std=c++17 core/daemon_main.cpp core/hardware_probe.cpp -lpdh -lwbemuuid -lole32 -loleaut32 -lpowrprof -lpsapi -o syspulse_daemon.exe
```

**MSVC:**
```bash
cl.exe /O2 /EHsc /std:c++17 core/daemon_main.cpp core/hardware_probe.cpp pdh.lib wbemuuid.lib ole32.lib oleaut32.lib powrprof.lib psapi.lib /Fe:syspulse_daemon.exe
```
