// Implementation details:
// Hardware probe declarations for querying Windows system metrics.
// Interfaces with Windows Performance Data Helper (PDH) for CPU utilization
// (both aggregate and per-core), disk I/O throughput, and network interface counters.
// Queries physical RAM via GlobalMemoryStatusEx, per-drive storage via GetDiskFreeSpaceExW,
// battery status via GetSystemPowerStatus, actual CPU frequency via CallNtPowerInformation,
// CPU temperature via WMI ACPI thermal zones through COM automation, and top-N resource
// consuming processes via performance snapshot differentials.

#ifndef SYSPULSE_HARDWARE_PROBE_H
#define SYSPULSE_HARDWARE_PROBE_H

#include "shared_memory.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <string>
#include <vector>

#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA 0x00000000L
#endif

#ifndef PDH_CSTATUS_NEW_DATA
#define PDH_CSTATUS_NEW_DATA 0x00000001L
#endif

// Snapshot of a single process CPU/memory state for delta calculations
struct ProcessSnapshot {
    uint32_t pid;
    std::string name;
    uint64_t kernel_time;   // FILETIME as 100ns ticks
    uint64_t user_time;     // FILETIME as 100ns ticks
    uint64_t memory_bytes;  // Working set size
};

class HardwareProbe {
public:
    HardwareProbe();
    ~HardwareProbe();

    bool initialize();
    void sample(SysPulseVitals& vitals);
    void shutdown();

private:
    // --- PDH query handles ---
    PDH_HQUERY cpuQuery;
    PDH_HCOUNTER cpuTotalCounter;
    PDH_HCOUNTER perCoreCounters[SYSPULSE_MAX_CORES];
    uint32_t detectedCoreCount;
    bool pdhInitialized;

    PDH_HQUERY diskQuery;
    PDH_HCOUNTER diskReadCounter;
    PDH_HCOUNTER diskWriteCounter;
    bool diskPdhInitialized;

    PDH_HQUERY netQuery;
    PDH_HCOUNTER netRecvCounter;
    PDH_HCOUNTER netSentCounter;
    bool netPdhInitialized;

    // --- Fallback CPU times ---
    ULARGE_INTEGER lastIdleTime;
    ULARGE_INTEGER lastKernelTime;
    ULARGE_INTEGER lastUserTime;

    // --- Timing ---
    uint64_t lastSampleTimeMs;

    // --- Process tracking ---
    std::vector<ProcessSnapshot> previousProcesses;
    uint64_t lastProcessSampleTime;  // For CPU delta calculation

    // --- WMI COM state ---
    bool wmiInitialized;
    void* pWbemLocator;   // IWbemLocator* (stored as void* to avoid COM headers in header)
    void* pWbemServices;  // IWbemServices*

    // --- Query methods ---
    double queryCpuUtilization();
    void queryPerCoreCpu(SysPulseVitals& vitals);
    void queryMemoryStatus(SysPulseVitals& vitals);
    void queryDiskThroughput(SysPulseVitals& vitals);
    void queryNetworkThroughput(SysPulseVitals& vitals);
    void queryDriveCapacity(SysPulseVitals& vitals);
    void queryBatteryStatus(SysPulseVitals& vitals);
    uint64_t queryActualCpuFrequency();
    void queryWmiTemperatures(SysPulseVitals& vitals);
    void queryTopProcesses(SysPulseVitals& vitals);
    uint32_t queryProcessCount();
    uint32_t querySystemUptime();

    // --- WMI helpers ---
    bool initializeWmi();
    void shutdownWmi();
};

#endif // SYSPULSE_HARDWARE_PROBE_H
