// Implementation details:
// Shared memory structure definition v2 for inter-process communication between
// the C++ native hardware collector and the Python web telemetry server.
// Expanded from v1 (128 bytes) to carry per-core CPU utilization arrays, per-drive
// storage capacity, top-N process resource consumers, battery/power status,
// and WMI-sourced thermal zone readings. Uses strict 8-byte packing alignment
// to ensure identical memory layout across MSVC/MinGW x64 and Python struct.unpack.

#ifndef SYSPULSE_SHARED_MEMORY_H
#define SYSPULSE_SHARED_MEMORY_H

#include <cstdint>

#define SYSPULSE_SHM_NAME L"Local\\SysPulse_Vitals"
#define SYSPULSE_MAGIC 0x53595350  // "SYSP" in ASCII hex
#define SYSPULSE_VERSION 2

// Maximum number of logical processor cores tracked individually
#define SYSPULSE_MAX_CORES 32

// Maximum number of mounted storage volumes tracked
#define SYSPULSE_MAX_DRIVES 8

// Maximum number of top resource-consuming processes tracked
#define SYSPULSE_MAX_PROCS 10

// Maximum length of a process name string (including null terminator)
#define SYSPULSE_PROC_NAME_LEN 64

// Maximum length of a drive mount path (e.g. "C:\", including null terminator)
#define SYSPULSE_DRIVE_PATH_LEN 8

#pragma pack(push, 8)

// Per-drive storage capacity snapshot
struct DriveInfo {
    char mount_path[SYSPULSE_DRIVE_PATH_LEN];  // Drive letter path (e.g. "C:\")
    uint64_t total_bytes;                       // Total partition capacity
    uint64_t free_bytes;                        // Available free space
};

// Per-process resource consumption snapshot
struct ProcessInfo {
    char name[SYSPULSE_PROC_NAME_LEN];  // Executable name (truncated to 63 chars)
    uint32_t pid;                        // Process identifier
    uint32_t _pad0;                      // Alignment padding to 8-byte boundary
    double cpu_percent;                  // CPU utilization percentage [0.0 - 100.0]
    uint64_t memory_bytes;               // Working set size in bytes
};

// Battery and power supply status
struct PowerInfo {
    uint8_t ac_connected;         // 1 if on AC power, 0 if on battery
    uint8_t battery_present;      // 1 if system has a battery, 0 for desktops
    uint8_t charging;             // 1 if currently charging
    uint8_t charge_percent;       // Battery charge level [0 - 100], 255 if unknown
    uint32_t seconds_remaining;   // Estimated battery seconds remaining, 0xFFFFFFFF if unknown
};

// Primary telemetry payload written by daemon, read by Python server
struct SysPulseVitals {
    // --- Header (16 bytes) ---
    uint32_t magic;               // Magic identifier to verify shared block validity
    uint32_t version;             // Schema version for backwards compatibility
    uint64_t timestamp_ms;        // Epoch timestamp in milliseconds of last sample

    // --- Aggregate CPU (32 bytes) ---
    double cpu_usage_percent;     // Total CPU utilization [0.0 - 100.0]
    double cpu_temp_celsius;      // CPU package temperature in degrees Celsius (WMI)
    uint64_t cpu_freq_mhz;        // Current maximum active core frequency in MHz
    uint32_t logical_core_count;  // Number of logical cores detected by the OS
    uint32_t _pad_cpu;            // Alignment padding

    // --- Per-Core CPU (SYSPULSE_MAX_CORES * 8 = 256 bytes) ---
    double per_core_usage[SYSPULSE_MAX_CORES];  // Per-logical-core utilization [0.0 - 100.0]

    // --- Memory (16 bytes) ---
    uint64_t ram_total_bytes;     // Total installed physical system memory
    uint64_t ram_used_bytes;      // Currently committed physical memory

    // --- GPU (40 bytes) ---
    double gpu_usage_percent;     // Core GPU 3D engine utilization [0.0 - 100.0]
    double gpu_temp_celsius;      // GPU core thermal junction in degrees Celsius
    uint64_t gpu_vram_total_bytes; // Total dedicated video memory
    uint64_t gpu_vram_used_bytes;  // Dedicated video memory currently allocated
    double gpu_power_watts;       // GPU board power draw in watts (from nvidia-smi)

    // --- Disk I/O (16 bytes) ---
    uint64_t disk_read_bytes_sec;   // Aggregated disk read throughput in bytes/sec
    uint64_t disk_write_bytes_sec;  // Aggregated disk write throughput in bytes/sec

    // --- Network I/O (16 bytes) ---
    uint64_t net_recv_bytes_sec;    // Primary network interface incoming bytes/sec
    uint64_t net_sent_bytes_sec;    // Primary network interface outgoing bytes/sec

    // --- System (8 bytes) ---
    uint32_t process_count;         // Total running OS processes
    uint32_t uptime_seconds;        // Windows kernel uptime in seconds

    // --- Power / Battery (8 bytes) ---
    PowerInfo power;

    // --- Per-Drive Storage (SYSPULSE_MAX_DRIVES * 24 = 192 bytes) ---
    uint32_t drive_count;           // Number of valid entries in drives[] array
    uint32_t _pad_drives;           // Alignment padding
    DriveInfo drives[SYSPULSE_MAX_DRIVES];

    // --- Top Processes (SYSPULSE_MAX_PROCS * 88 = 880 bytes) ---
    uint32_t top_proc_count;        // Number of valid entries in top_procs[] array
    uint32_t _pad_procs;            // Alignment padding
    ProcessInfo top_procs[SYSPULSE_MAX_PROCS];

    // --- WMI Thermal Zones (up to 4 zones, 32 bytes) ---
    uint32_t thermal_zone_count;    // Number of valid thermal zone readings
    uint32_t _pad_thermal;          // Alignment padding
    double thermal_zones[4];        // Temperature readings from WMI ACPI thermal zones
};

#pragma pack(pop)

// Compile-time struct size verification (update expected size after layout changes)
// v2 total: 16 + 32 + 256 + 16 + 40 + 16 + 16 + 8 + 8 + (8 + 192) + (8 + 880) + (8 + 32) = 1536 bytes
static_assert(sizeof(SysPulseVitals) == 1536, "SysPulseVitals v2 struct layout must be exactly 1536 bytes");

#endif // SYSPULSE_SHARED_MEMORY_H
