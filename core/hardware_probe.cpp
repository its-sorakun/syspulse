// Implementation details:
// Windows hardware probe implementation.
// Acquires CPU utilization via PDH with fallback to kernel system times calculation.
// Collects memory pressure metrics through GlobalMemoryStatusEx and process counts
// via Toolhelp32 process snapshotting. Uses high-resolution system timers for delta calculations.

#include "include/hardware_probe.h"
#include <tlhelp32.h>
#include <chrono>
#include <iostream>

#pragma comment(lib, "pdh.lib")

HardwareProbe::HardwareProbe()
    : cpuQuery(nullptr),
      cpuTotalCounter(nullptr),
      pdhInitialized(false),
      lastSampleTimeMs(0),
      lastDiskReadBytes(0),
      lastDiskWriteBytes(0),
      lastNetRecvBytes(0),
      lastNetSentBytes(0) {
    lastIdleTime.QuadPart = 0;
    lastKernelTime.QuadPart = 0;
    lastUserTime.QuadPart = 0;
}

HardwareProbe::~HardwareProbe() {
    shutdown();
}

bool HardwareProbe::initialize() {
    // Attempt PDH counter initialization for standard processor time query
    PDH_STATUS status = PdhOpenQuery(NULL, 0, &cpuQuery);
    if (status == ERROR_SUCCESS) {
        status = PdhAddEnglishCounterW(
            cpuQuery,
            L"\\Processor(_Total)\\% Processor Time",
            0,
            &cpuTotalCounter
        );
        if (status == ERROR_SUCCESS) {
            PdhCollectQueryData(cpuQuery);
            pdhInitialized = true;
        } else {
            PdhCloseQuery(cpuQuery);
            cpuQuery = nullptr;
        }
    }

    // Seed kernel time snapshot for fallback calculation
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        lastIdleTime.LowPart = idle.dwLowDateTime;
        lastIdleTime.HighPart = idle.dwHighDateTime;
        lastKernelTime.LowPart = kernel.dwLowDateTime;
        lastKernelTime.HighPart = kernel.dwHighDateTime;
        lastUserTime.LowPart = user.dwLowDateTime;
        lastUserTime.HighPart = user.dwHighDateTime;
    }

    auto now = std::chrono::system_clock::now();
    lastSampleTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    return true;
}

double HardwareProbe::queryCpuUtilization() {
    if (pdhInitialized && cpuQuery && cpuTotalCounter) {
        PDH_STATUS status = PdhCollectQueryData(cpuQuery);
        if (status == ERROR_SUCCESS) {
            PDH_FMT_COUNTERVALUE counterVal;
            status = PdhGetFormattedCounterValue(cpuTotalCounter, PDH_FMT_DOUBLE, NULL, &counterVal);
            if (status == ERROR_SUCCESS && (counterVal.CStatus == PDH_CSTATUS_VALID_DATA || counterVal.CStatus == PDH_CSTATUS_NEW_DATA)) {
                double val = counterVal.doubleValue;
                if (val < 0.0) val = 0.0;
                if (val > 100.0) val = 100.0;
                return val;
            }
        }
    }

    // Fallback: Compute CPU load differential via GetSystemTimes
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return 0.0;
    }

    ULARGE_INTEGER curIdle, curKernel, curUser;
    curIdle.LowPart = idle.dwLowDateTime;
    curIdle.HighPart = idle.dwHighDateTime;
    curKernel.LowPart = kernel.dwLowDateTime;
    curKernel.HighPart = kernel.dwHighDateTime;
    curUser.LowPart = user.dwLowDateTime;
    curUser.HighPart = user.dwHighDateTime;

    uint64_t diffKernel = curKernel.QuadPart - lastKernelTime.QuadPart;
    uint64_t diffUser = curUser.QuadPart - lastUserTime.QuadPart;
    uint64_t diffIdle = curIdle.QuadPart - lastIdleTime.QuadPart;

    lastKernelTime = curKernel;
    lastUserTime = curUser;
    lastIdleTime = curIdle;

    uint64_t totalSys = diffKernel + diffUser;
    if (totalSys == 0) return 0.0;

    double usage = (double)(totalSys - diffIdle) * 100.0 / (double)totalSys;
    if (usage < 0.0) usage = 0.0;
    if (usage > 100.0) usage = 100.0;
    return usage;
}

void HardwareProbe::queryMemoryStatus(SysPulseVitals& vitals) {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        vitals.ram_total_bytes = memInfo.ullTotalPhys;
        vitals.ram_used_bytes = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
    }
}

uint32_t HardwareProbe::queryProcessCount() {
    uint32_t count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(PROCESSENTRY32W);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                count++;
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return count;
}

uint32_t HardwareProbe::querySystemUptime() {
    return static_cast<uint32_t>(GetTickCount64() / 1000);
}

double HardwareProbe::queryCpuTemperature() {
    // Queries named shared memory exposed by low-level SMU/driver daemon if present,
    // otherwise returns 0.0 indicating thermal sensor driver offline.
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Kiko_CPU_Temp");
    if (hMap != NULL) {
        const double* pTemp = static_cast<const double*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(double)));
        if (pTemp != nullptr) {
            double tempVal = *pTemp;
            UnmapViewOfFile(pTemp);
            CloseHandle(hMap);
            if (tempVal > 0.0 && tempVal < 125.0) {
                return tempVal;
            }
        }
        CloseHandle(hMap);
    }
    return 0.0;
}

void HardwareProbe::sample(SysPulseVitals& vitals) {
    auto now = std::chrono::system_clock::now();
    uint64_t currentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    double deltaSec = (currentMs - lastSampleTimeMs) / 1000.0;
    if (deltaSec <= 0.0) deltaSec = 1.0;
    lastSampleTimeMs = currentMs;

    vitals.magic = SYSPULSE_MAGIC;
    vitals.version = SYSPULSE_VERSION;
    vitals.timestamp_ms = currentMs;

    vitals.cpu_usage_percent = queryCpuUtilization();
    vitals.cpu_temp_celsius = queryCpuTemperature();
    vitals.cpu_freq_mhz = 3600; // Baseline nominal clock frequency
    queryMemoryStatus(vitals);

    vitals.process_count = queryProcessCount();
    vitals.uptime_seconds = querySystemUptime();
}

void HardwareProbe::shutdown() {
    if (cpuQuery) {
        PdhCloseQuery(cpuQuery);
        cpuQuery = nullptr;
        cpuTotalCounter = nullptr;
        pdhInitialized = false;
    }
}
