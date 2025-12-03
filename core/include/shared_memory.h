// Implementation details:
// Shared memory structure definition for inter-process communication between
// the C++ native hardware collector and the Python web telemetry server.
// Uses strict 64-bit alignment and packing to ensure identical memory layout
// across MSVC x64 compilation units and Python struct unpack offsets.

#ifndef SYSPULSE_SHARED_MEMORY_H
#define SYSPULSE_SHARED_MEMORY_H

#include <cstdint>

#define SYSPULSE_SHM_NAME L"Local\\SysPulse_Vitals"
#define SYSPULSE_MAGIC 0x53595350 // "SYSP" in ASCII hex
#define SYSPULSE_VERSION 1

#pragma pack(push, 8)
struct SysPulseVitals {
    uint32_t magic;                 // Magic identifier to verify shared block validity
    uint32_t version;               // Schema version for backwards compatibility
    uint64_t timestamp_ms;          // Epoch timestamp in milliseconds of last sample
    double cpu_usage_percent;       // Total CPU utilization [0.0 - 100.0]
    double cpu_temp_celsius;        // CPU package temperature in degrees Celsius
    uint64_t cpu_freq_mhz;          // Current active CPU core clock frequency in MHz
    uint64_t ram_total_bytes;       // Total installed physical system memory
    uint64_t ram_used_bytes;        // Currently committed physical memory
    double gpu_usage_percent;       // Core GPU 3D engine utilization [0.0 - 100.0]
    double gpu_temp_celsius;        // GPU core thermal junction in degrees Celsius
    uint64_t gpu_vram_total_bytes;  // Total dedicated video memory
    uint64_t gpu_vram_used_bytes;   // Dedicated video memory currently allocated
    uint64_t disk_read_bytes_sec;   // Aggregated disk read throughput in bytes/sec
    uint64_t disk_write_bytes_sec;  // Aggregated disk write throughput in bytes/sec
    uint64_t net_recv_bytes_sec;    // Primary network interface incoming bytes/sec
    uint64_t net_sent_bytes_sec;    // Primary network interface outgoing bytes/sec
    uint32_t process_count;         // Total running OS processes
    uint32_t uptime_seconds;        // Windows kernel uptime in seconds
};
#pragma pack(pop)

static_assert(sizeof(SysPulseVitals) == 128, "SysPulseVitals struct layout must be exactly 128 bytes");

#endif // SYSPULSE_SHARED_MEMORY_H
