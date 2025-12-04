// Implementation details:
// Hardware probe declarations for querying Windows system metrics.
// Interfaces with Windows Performance Data Helper (PDH) for CPU utilization,
// GlobalMemoryStatusEx for physical RAM, and NtQuerySystemInformation / GetSystemTimes.

#ifndef SYSPULSE_HARDWARE_PROBE_H
#define SYSPULSE_HARDWARE_PROBE_H

#include "shared_memory.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA 0x00000000L
#endif

#ifndef PDH_CSTATUS_NEW_DATA
#define PDH_CSTATUS_NEW_DATA 0x00000001L
#endif

class HardwareProbe {
public:
    HardwareProbe();
    ~HardwareProbe();

    bool initialize();
    void sample(SysPulseVitals& vitals);
    void shutdown();

private:
    PDH_HQUERY cpuQuery;
    PDH_HCOUNTER cpuTotalCounter;
    bool pdhInitialized;

    ULARGE_INTEGER lastIdleTime;
    ULARGE_INTEGER lastKernelTime;
    ULARGE_INTEGER lastUserTime;

    uint64_t lastSampleTimeMs;
    uint64_t lastDiskReadBytes;
    uint64_t lastDiskWriteBytes;
    uint64_t lastNetRecvBytes;
    uint64_t lastNetSentBytes;

    double queryCpuUtilization();
    void queryMemoryStatus(SysPulseVitals& vitals);
    void queryDiskThroughput(SysPulseVitals& vitals, double deltaSec);
    void queryNetworkThroughput(SysPulseVitals& vitals, double deltaSec);
    uint32_t queryProcessCount();
    uint32_t querySystemUptime();
    double queryCpuTemperature();
};

#endif // SYSPULSE_HARDWARE_PROBE_H
