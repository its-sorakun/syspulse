# Implementation details:
# Telemetry collector interfacing with the C++ native daemon's shared memory block.
# Reads 128-byte packed struct from Windows paging memory via mmap.
# Implements pure-Python fallback probes (ctypes GlobalMemoryStatusEx and kernel times)
# when the native daemon is offline, ensuring continuous metrics availability.

import mmap
import struct
import time
import ctypes
from ctypes import wintypes
from typing import Dict, Any, Optional

# Struct format matching shared_memory.h:
# <IIQddQQddQQQQQQII (128 bytes total)
# magic, version, timestamp_ms, cpu_usage, cpu_temp, cpu_freq,
# ram_total, ram_used, gpu_usage, gpu_temp, vram_total, vram_used,
# disk_read, disk_write, net_recv, net_sent, process_count, uptime
SHM_FORMAT = "<IIQddQQddQQQQQQII"
SHM_SIZE = struct.calcsize(SHM_FORMAT)
SHM_NAME = "Local\\SysPulse_Vitals"
SYSPULSE_MAGIC = 0x53595350


class NativeMemoryStatus(ctypes.Structure):
    _fields_ = [
        ("dwLength", wintypes.DWORD),
        ("dwMemoryLoad", wintypes.DWORD),
        ("ullTotalPhys", ctypes.c_uint64),
        ("ullAvailPhys", ctypes.c_uint64),
        ("ullTotalPageFile", ctypes.c_uint64),
        ("ullAvailPageFile", ctypes.c_uint64),
        ("ullTotalVirtual", ctypes.c_uint64),
        ("ullAvailVirtual", ctypes.c_uint64),
        ("ullAvailExtendedVirtual", ctypes.c_uint64),
    ]


class TelemetryCollector:
    def __init__(self):
        self._last_idle_time = 0
        self._last_kernel_time = 0
        self._last_user_time = 0
        self._init_cpu_times()

    def _init_cpu_times(self) -> None:
        idle = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        if ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
            self._last_idle_time = (idle.dwHighDateTime << 32) | idle.dwLowDateTime
            self._last_kernel_time = (kernel.dwHighDateTime << 32) | kernel.dwLowDateTime
            self._last_user_time = (user.dwHighDateTime << 32) | user.dwLowDateTime

    def read_shared_memory(self) -> Optional[Dict[str, Any]]:
        try:
            shm = mmap.mmap(-1, SHM_SIZE, tagname=SHM_NAME, access=mmap.ACCESS_READ)
            raw = shm.read(SHM_SIZE)
            shm.close()

            if len(raw) != SHM_SIZE:
                return None

            unpacked = struct.unpack(SHM_FORMAT, raw)
            magic = unpacked[0]
            if magic != SYSPULSE_MAGIC:
                return None

            return {
                "source": "native_daemon",
                "timestamp_ms": unpacked[2],
                "cpu_usage": round(unpacked[3], 1),
                "cpu_temp": round(unpacked[4], 1) if unpacked[4] > 0.0 else None,
                "cpu_freq_mhz": unpacked[5],
                "ram_total_bytes": unpacked[6],
                "ram_used_bytes": unpacked[7],
                "gpu_usage": round(unpacked[8], 1),
                "gpu_temp": round(unpacked[9], 1) if unpacked[9] > 0.0 else None,
                "vram_total_bytes": unpacked[10],
                "vram_used_bytes": unpacked[11],
                "disk_read_bytes_sec": unpacked[12],
                "disk_write_bytes_sec": unpacked[13],
                "net_recv_bytes_sec": unpacked[14],
                "net_sent_bytes_sec": unpacked[15],
                "process_count": unpacked[16],
                "uptime_seconds": unpacked[17],
            }
        except (WindowsError, OSError):
            return None

    def read_native_fallback(self) -> Dict[str, Any]:
        # Fallback query using direct Win32 kernel API calls
        idle = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()

        cpu_usage = 0.0
        if ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
            cur_idle = (idle.dwHighDateTime << 32) | idle.dwLowDateTime
            cur_kernel = (kernel.dwHighDateTime << 32) | kernel.dwLowDateTime
            cur_user = (user.dwHighDateTime << 32) | user.dwLowDateTime

            diff_idle = cur_idle - self._last_idle_time
            diff_kernel = cur_kernel - self._last_kernel_time
            diff_user = cur_user - self._last_user_time

            self._last_idle_time = cur_idle
            self._last_kernel_time = cur_kernel
            self._last_user_time = cur_user

            total = diff_kernel + diff_user
            if total > 0:
                cpu_usage = max(0.0, min(100.0, ((total - diff_idle) / total) * 100.0))

        mem = NativeMemoryStatus()
        mem.dwLength = ctypes.sizeof(NativeMemoryStatus)
        ram_total = 0
        ram_used = 0
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(mem)):
            ram_total = mem.ullTotalPhys
            ram_used = mem.ullTotalPhys - mem.ullAvailPhys

        uptime = int(ctypes.windll.kernel32.GetTickCount64() / 1000)

        # Check for secondary SMU temperature mapping if present
        cpu_temp = None
        try:
            temp_shm = mmap.mmap(-1, 8, tagname="Kiko_CPU_Temp", access=mmap.ACCESS_READ)
            t_bytes = temp_shm.read(8)
            temp_shm.close()
            t_val = struct.unpack("d", t_bytes)[0]
            if 0.0 < t_val < 125.0:
                cpu_temp = round(t_val, 1)
        except Exception:
            pass

        return {
            "source": "native_fallback",
            "timestamp_ms": int(time.time() * 1000),
            "cpu_usage": round(cpu_usage, 1),
            "cpu_temp": cpu_temp,
            "cpu_freq_mhz": 3600,
            "ram_total_bytes": ram_total,
            "ram_used_bytes": ram_used,
            "gpu_usage": 0.0,
            "gpu_temp": None,
            "vram_total_bytes": 0,
            "vram_used_bytes": 0,
            "disk_read_bytes_sec": 0,
            "disk_write_bytes_sec": 0,
            "net_recv_bytes_sec": 0,
            "net_sent_bytes_sec": 0,
            "process_count": 0,
            "uptime_seconds": uptime,
        }

    def collect(self) -> Dict[str, Any]:
        data = self.read_shared_memory()
        if data is None:
            data = self.read_native_fallback()
        return data
