// Implementation details:
// Full Windows hardware probe implementation covering all telemetry categories.
//
// CPU utilization: PDH aggregate counter (\Processor(_Total)\% Processor Time) with
// GetSystemTimes differential fallback. Per-core counters via PDH \Processor(N) queries.
// Actual core frequency via undocumented CallNtPowerInformation(ProcessorInformation).
//
// Disk I/O: PDH \PhysicalDisk(_Total) read/write byte counters.
// Network I/O: PDH \Network Interface(*) byte counters (first detected interface).
// Per-drive storage: GetDiskFreeSpaceExW iterating GetLogicalDriveStringsW volumes.
// Battery: GetSystemPowerStatus for AC/charge/remaining time on laptops.
// Temperature: WMI COM automation querying MSAcpi_ThermalZoneTemperature from root\WMI
// namespace (requires admin elevation on most systems, gracefully degrades if unavailable).
// Top-N processes: Toolhelp32 snapshot with OpenProcess + GetProcessTimes for per-process
// CPU delta calculation, sorted by descending CPU percentage.

#include "include/hardware_probe.h"
#include <tlhelp32.h>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <cstring>

// WMI / COM includes for thermal zone queries
#include <comdef.h>
#include <Wbemidl.h>

// Process memory info for top-N process tracking
#include <psapi.h>

// NtPowerInformation for actual CPU frequency
#include <ntstatus.h>
#include <powerbase.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "PowrProf.lib")

// PROCESSOR_POWER_INFORMATION structure returned by CallNtPowerInformation.
// Not declared in MinGW headers, defined here from the Windows DDK documentation.
typedef struct _PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION;

HardwareProbe::HardwareProbe()
    : cpuQuery(nullptr),
      cpuTotalCounter(nullptr),
      detectedCoreCount(0),
      pdhInitialized(false),
      diskQuery(nullptr),
      diskReadCounter(nullptr),
      diskWriteCounter(nullptr),
      diskPdhInitialized(false),
      netQuery(nullptr),
      netRecvCounter(nullptr),
      netSentCounter(nullptr),
      netPdhInitialized(false),
      lastSampleTimeMs(0),
      lastProcessSampleTime(0),
      wmiInitialized(false),
      pWbemLocator(nullptr),
      pWbemServices(nullptr) {
    lastIdleTime.QuadPart = 0;
    lastKernelTime.QuadPart = 0;
    lastUserTime.QuadPart = 0;
    std::memset(perCoreCounters, 0, sizeof(perCoreCounters));
}

HardwareProbe::~HardwareProbe() {
    shutdown();
}

bool HardwareProbe::initialize() {
    // --- CPU aggregate PDH counter ---
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

    // --- Per-core PDH counters ---
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    detectedCoreCount = sysInfo.dwNumberOfProcessors;
    if (detectedCoreCount > SYSPULSE_MAX_CORES) {
        detectedCoreCount = SYSPULSE_MAX_CORES;
    }

    if (pdhInitialized && cpuQuery) {
        for (uint32_t i = 0; i < detectedCoreCount; i++) {
            wchar_t counterPath[128];
            swprintf(counterPath, 128, L"\\Processor(%u)\\%% Processor Time", i);
            PDH_STATUS coreStatus = PdhAddEnglishCounterW(
                cpuQuery, counterPath, 0, &perCoreCounters[i]
            );
            if (coreStatus != ERROR_SUCCESS) {
                perCoreCounters[i] = nullptr;
            }
        }
    }

    // --- Disk I/O PDH counters ---
    status = PdhOpenQuery(NULL, 0, &diskQuery);
    if (status == ERROR_SUCCESS) {
        PDH_STATUS readStatus = PdhAddEnglishCounterW(
            diskQuery,
            L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
            0,
            &diskReadCounter
        );
        PDH_STATUS writeStatus = PdhAddEnglishCounterW(
            diskQuery,
            L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
            0,
            &diskWriteCounter
        );
        if (readStatus == ERROR_SUCCESS && writeStatus == ERROR_SUCCESS) {
            PdhCollectQueryData(diskQuery);
            diskPdhInitialized = true;
        } else {
            PdhCloseQuery(diskQuery);
            diskQuery = nullptr;
            diskReadCounter = nullptr;
            diskWriteCounter = nullptr;
        }
    }

    // --- Network I/O PDH counters ---
    // Uses wildcard (*) to capture the first available network interface.
    // PDH automatically resolves the wildcard to installed interfaces.
    status = PdhOpenQuery(NULL, 0, &netQuery);
    if (status == ERROR_SUCCESS) {
        PDH_STATUS recvStatus = PdhAddEnglishCounterW(
            netQuery,
            L"\\Network Interface(*)\\Bytes Received/sec",
            0,
            &netRecvCounter
        );
        PDH_STATUS sentStatus = PdhAddEnglishCounterW(
            netQuery,
            L"\\Network Interface(*)\\Bytes Sent/sec",
            0,
            &netSentCounter
        );
        if (recvStatus == ERROR_SUCCESS && sentStatus == ERROR_SUCCESS) {
            PdhCollectQueryData(netQuery);
            netPdhInitialized = true;
        } else {
            PdhCloseQuery(netQuery);
            netQuery = nullptr;
            netRecvCounter = nullptr;
            netSentCounter = nullptr;
        }
    }

    // Seed kernel time snapshot for fallback CPU calculation
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
    lastProcessSampleTime = lastSampleTimeMs;

    // Initialize WMI for thermal zone queries (may fail without admin)
    wmiInitialized = initializeWmi();

    return true;
}

// ============================================================
// CPU Utilization (Aggregate)
// ============================================================
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

// ============================================================
// Per-Core CPU Utilization
// ============================================================
void HardwareProbe::queryPerCoreCpu(SysPulseVitals& vitals) {
    vitals.logical_core_count = detectedCoreCount;

    // Zero out all slots first
    for (uint32_t i = 0; i < SYSPULSE_MAX_CORES; i++) {
        vitals.per_core_usage[i] = 0.0;
    }

    if (!pdhInitialized) return;

    // PDH data was already collected in queryCpuUtilization(), read per-core values
    for (uint32_t i = 0; i < detectedCoreCount; i++) {
        if (perCoreCounters[i] == nullptr) continue;

        PDH_FMT_COUNTERVALUE counterVal;
        PDH_STATUS status = PdhGetFormattedCounterValue(
            perCoreCounters[i], PDH_FMT_DOUBLE, NULL, &counterVal
        );
        if (status == ERROR_SUCCESS &&
            (counterVal.CStatus == PDH_CSTATUS_VALID_DATA || counterVal.CStatus == PDH_CSTATUS_NEW_DATA)) {
            double val = counterVal.doubleValue;
            if (val < 0.0) val = 0.0;
            if (val > 100.0) val = 100.0;
            vitals.per_core_usage[i] = val;
        }
    }
}

// ============================================================
// Memory
// ============================================================
void HardwareProbe::queryMemoryStatus(SysPulseVitals& vitals) {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        vitals.ram_total_bytes = memInfo.ullTotalPhys;
        vitals.ram_used_bytes = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
    }
}

// ============================================================
// Disk I/O Throughput (PDH)
// ============================================================
void HardwareProbe::queryDiskThroughput(SysPulseVitals& vitals) {
    if (!diskPdhInitialized || !diskQuery) {
        vitals.disk_read_bytes_sec = 0;
        vitals.disk_write_bytes_sec = 0;
        return;
    }

    PdhCollectQueryData(diskQuery);

    PDH_FMT_COUNTERVALUE readVal, writeVal;
    PDH_STATUS rStatus = PdhGetFormattedCounterValue(diskReadCounter, PDH_FMT_LARGE, NULL, &readVal);
    PDH_STATUS wStatus = PdhGetFormattedCounterValue(diskWriteCounter, PDH_FMT_LARGE, NULL, &writeVal);

    vitals.disk_read_bytes_sec = (rStatus == ERROR_SUCCESS) ? (uint64_t)readVal.largeValue : 0;
    vitals.disk_write_bytes_sec = (wStatus == ERROR_SUCCESS) ? (uint64_t)writeVal.largeValue : 0;
}

// ============================================================
// Network I/O Throughput (PDH)
// ============================================================
void HardwareProbe::queryNetworkThroughput(SysPulseVitals& vitals) {
    if (!netPdhInitialized || !netQuery) {
        vitals.net_recv_bytes_sec = 0;
        vitals.net_sent_bytes_sec = 0;
        return;
    }

    PdhCollectQueryData(netQuery);

    PDH_FMT_COUNTERVALUE recvVal, sentVal;
    PDH_STATUS rStatus = PdhGetFormattedCounterValue(netRecvCounter, PDH_FMT_LARGE, NULL, &recvVal);
    PDH_STATUS sStatus = PdhGetFormattedCounterValue(netSentCounter, PDH_FMT_LARGE, NULL, &sentVal);

    vitals.net_recv_bytes_sec = (rStatus == ERROR_SUCCESS) ? (uint64_t)recvVal.largeValue : 0;
    vitals.net_sent_bytes_sec = (sStatus == ERROR_SUCCESS) ? (uint64_t)sentVal.largeValue : 0;
}

// ============================================================
// Per-Drive Storage Capacity
// ============================================================
void HardwareProbe::queryDriveCapacity(SysPulseVitals& vitals) {
    // Get all logical drive strings (e.g. "C:\" "D:\" ...)
    char driveStrings[256];
    DWORD len = GetLogicalDriveStringsA(sizeof(driveStrings), driveStrings);

    vitals.drive_count = 0;
    if (len == 0 || len > sizeof(driveStrings)) return;

    char* current = driveStrings;
    while (*current && vitals.drive_count < SYSPULSE_MAX_DRIVES) {
        UINT driveType = GetDriveTypeA(current);

        // Only report fixed drives and removable drives, skip network/cdrom/ramdisk
        if (driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE) {
            ULARGE_INTEGER freeBytesAvailable, totalBytes, totalFreeBytes;
            if (GetDiskFreeSpaceExA(current, &freeBytesAvailable, &totalBytes, &totalFreeBytes)) {
                DriveInfo& drv = vitals.drives[vitals.drive_count];
                std::strncpy(drv.mount_path, current, SYSPULSE_DRIVE_PATH_LEN - 1);
                drv.mount_path[SYSPULSE_DRIVE_PATH_LEN - 1] = '\0';
                drv.total_bytes = totalBytes.QuadPart;
                drv.free_bytes = totalFreeBytes.QuadPart;
                vitals.drive_count++;
            }
        }

        // Advance past null terminator to next drive string
        current += std::strlen(current) + 1;
    }
}

// ============================================================
// Battery / Power Status
// ============================================================
void HardwareProbe::queryBatteryStatus(SysPulseVitals& vitals) {
    SYSTEM_POWER_STATUS powerStatus;
    if (!GetSystemPowerStatus(&powerStatus)) {
        vitals.power.ac_connected = 1;
        vitals.power.battery_present = 0;
        vitals.power.charging = 0;
        vitals.power.charge_percent = 255;
        vitals.power.seconds_remaining = 0xFFFFFFFF;
        return;
    }

    vitals.power.ac_connected = (powerStatus.ACLineStatus == 1) ? 1 : 0;

    // BatteryFlag == 128 means no system battery
    vitals.power.battery_present = (powerStatus.BatteryFlag != 128) ? 1 : 0;

    // BatteryFlag bit 3 (value 8) indicates charging
    vitals.power.charging = (powerStatus.BatteryFlag & 8) ? 1 : 0;

    // BatteryLifePercent: 0-100, or 255 if unknown
    vitals.power.charge_percent = powerStatus.BatteryLifePercent;

    // BatteryLifeTime: seconds of remaining battery, or -1 (0xFFFFFFFF) if unknown/on AC
    vitals.power.seconds_remaining = powerStatus.BatteryLifeTime;
}

// ============================================================
// Actual CPU Frequency via NtPowerInformation
// ============================================================
uint64_t HardwareProbe::queryActualCpuFrequency() {
    // CallNtPowerInformation with ProcessorInformation (info class 11) returns
    // PROCESSOR_POWER_INFORMATION for each logical processor. Reading CurrentMhz
    // gives the actual frequency the OS is running the core at, accounting for
    // turbo boost, thermal throttling, and power management states.
    //
    // This is more accurate than registry HKLM\...\CentralProcessor\0\~MHz
    // which only reports the base/nominal frequency.

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD numProcs = sysInfo.dwNumberOfProcessors;

    size_t bufferSize = sizeof(PROCESSOR_POWER_INFORMATION) * numProcs;
    std::vector<uint8_t> buffer(bufferSize, 0);

    // ProcessorInformation = 11
    NTSTATUS status = CallNtPowerInformation(
        (POWER_INFORMATION_LEVEL)11,
        NULL, 0,
        buffer.data(), (ULONG)bufferSize
    );

    if (status != 0) {
        return 3600;  // Fallback nominal frequency if API call fails
    }

    // Find the maximum CurrentMhz across all processors (accounts for big.LITTLE
    // and mixed P-core/E-core architectures like Intel 12th gen+)
    auto* info = reinterpret_cast<PROCESSOR_POWER_INFORMATION*>(buffer.data());
    uint64_t maxMhz = 0;
    for (DWORD i = 0; i < numProcs; i++) {
        if (info[i].CurrentMhz > maxMhz) {
            maxMhz = info[i].CurrentMhz;
        }
    }

    return (maxMhz > 0) ? maxMhz : 3600;
}

// ============================================================
// WMI Initialization (COM Automation)
// ============================================================
bool HardwareProbe::initializeWmi() {
    // CoInitializeEx with COINIT_MULTITHREADED for background daemon usage.
    // WMI thermal zone queries require root\WMI namespace access which
    // typically needs administrator elevation. If unavailable, thermal
    // readings gracefully fall back to 0.0.

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    hr = CoInitializeSecurity(
        NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE, NULL
    );

    IWbemLocator* pLocator = nullptr;
    hr = CoCreateInstance(
        CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, reinterpret_cast<void**>(&pLocator)
    );
    if (FAILED(hr)) {
        return false;
    }

    IWbemServices* pServices = nullptr;
    hr = pLocator->ConnectServer(
        _bstr_t(L"ROOT\\WMI"),
        NULL, NULL, 0, 0, NULL, NULL, &pServices
    );
    if (FAILED(hr)) {
        pLocator->Release();
        return false;
    }

    // Set proxy blanket so WMI calls use current thread credentials
    hr = CoSetProxyBlanket(
        pServices,
        RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE
    );

    if (FAILED(hr)) {
        pServices->Release();
        pLocator->Release();
        return false;
    }

    pWbemLocator = pLocator;
    pWbemServices = pServices;
    return true;
}

void HardwareProbe::shutdownWmi() {
    if (pWbemServices) {
        static_cast<IWbemServices*>(pWbemServices)->Release();
        pWbemServices = nullptr;
    }
    if (pWbemLocator) {
        static_cast<IWbemLocator*>(pWbemLocator)->Release();
        pWbemLocator = nullptr;
    }
    if (wmiInitialized) {
        CoUninitialize();
        wmiInitialized = false;
    }
}

// ============================================================
// WMI Thermal Zone Temperatures
// ============================================================
void HardwareProbe::queryWmiTemperatures(SysPulseVitals& vitals) {
    vitals.thermal_zone_count = 0;
    for (int i = 0; i < 4; i++) vitals.thermal_zones[i] = 0.0;

    if (!wmiInitialized || !pWbemServices) return;

    IWbemServices* pSvc = static_cast<IWbemServices*>(pWbemServices);
    IEnumWbemClassObject* pEnumerator = nullptr;

    // MSAcpi_ThermalZoneTemperature returns temperature in tenths of Kelvin.
    // Conversion: Celsius = (value / 10.0) - 273.15
    HRESULT hr = pSvc->ExecQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL, &pEnumerator
    );

    if (FAILED(hr)) return;

    IWbemClassObject* pObj = nullptr;
    ULONG returned = 0;
    uint32_t zoneIndex = 0;

    while (pEnumerator->Next(WBEM_INFINITE, 1, &pObj, &returned) == S_OK && zoneIndex < 4) {
        VARIANT vtTemp;
        VariantInit(&vtTemp);

        hr = pObj->Get(L"CurrentTemperature", 0, &vtTemp, NULL, NULL);
        if (SUCCEEDED(hr) && vtTemp.vt == VT_I4) {
            // Convert from tenths of Kelvin to Celsius
            double celsius = (vtTemp.lVal / 10.0) - 273.15;
            if (celsius > 0.0 && celsius < 150.0) {
                vitals.thermal_zones[zoneIndex] = celsius;
                zoneIndex++;
            }
        }

        VariantClear(&vtTemp);
        pObj->Release();
    }

    vitals.thermal_zone_count = zoneIndex;
    pEnumerator->Release();

    // Use first thermal zone as CPU package temperature if available
    if (zoneIndex > 0 && vitals.cpu_temp_celsius <= 0.0) {
        vitals.cpu_temp_celsius = vitals.thermal_zones[0];
    }
}

// ============================================================
// Top-N Process Resource Consumers
// ============================================================
void HardwareProbe::queryTopProcesses(SysPulseVitals& vitals) {
    vitals.top_proc_count = 0;

    auto now = std::chrono::system_clock::now();
    uint64_t currentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    double deltaSec = (currentMs - lastProcessSampleTime) / 1000.0;
    if (deltaSec <= 0.001) deltaSec = 1.0;

    // Snapshot all running processes
    std::vector<ProcessSnapshot> currentProcesses;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessSnapshot ps;
            ps.pid = entry.th32ProcessID;

            // Convert wide process name to narrow string
            char narrowName[SYSPULSE_PROC_NAME_LEN];
            WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1,
                                narrowName, SYSPULSE_PROC_NAME_LEN, NULL, NULL);
            ps.name = narrowName;

            // Open process handle to query timing information
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ps.pid);
            if (hProc != NULL) {
                FILETIME ftCreate, ftExit, ftKernel, ftUser;
                if (GetProcessTimes(hProc, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                    ULARGE_INTEGER kTime, uTime;
                    kTime.LowPart = ftKernel.dwLowDateTime;
                    kTime.HighPart = ftKernel.dwHighDateTime;
                    uTime.LowPart = ftUser.dwLowDateTime;
                    uTime.HighPart = ftUser.dwHighDateTime;
                    ps.kernel_time = kTime.QuadPart;
                    ps.user_time = uTime.QuadPart;
                } else {
                    ps.kernel_time = 0;
                    ps.user_time = 0;
                }

                // Query working set size for memory consumption
                PROCESS_MEMORY_COUNTERS_EX memCounters;
                memCounters.cb = sizeof(memCounters);
                if (GetProcessMemoryInfo(hProc, (PROCESS_MEMORY_COUNTERS*)&memCounters, sizeof(memCounters))) {
                    ps.memory_bytes = memCounters.WorkingSetSize;
                } else {
                    ps.memory_bytes = 0;
                }

                CloseHandle(hProc);
            } else {
                ps.kernel_time = 0;
                ps.user_time = 0;
                ps.memory_bytes = 0;
            }

            currentProcesses.push_back(ps);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    // Calculate per-process CPU percentage by comparing against previous snapshot.
    // CPU time delta (in 100ns ticks) divided by wall-clock delta gives utilization.
    // Dividing by logical core count normalizes to [0..100%] system-wide share.
    struct ProcessCpuInfo {
        std::string name;
        uint32_t pid;
        double cpu_percent;
        uint64_t memory_bytes;
    };
    std::vector<ProcessCpuInfo> cpuInfos;

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD numCores = sysInfo.dwNumberOfProcessors;

    for (const auto& cur : currentProcesses) {
        double cpuPercent = 0.0;

        // Find matching PID in previous snapshot for delta calculation
        for (const auto& prev : previousProcesses) {
            if (prev.pid == cur.pid && prev.name == cur.name) {
                uint64_t prevTotal = prev.kernel_time + prev.user_time;
                uint64_t curTotal = cur.kernel_time + cur.user_time;
                if (curTotal > prevTotal) {
                    uint64_t cpuDelta = curTotal - prevTotal;
                    // Convert 100ns ticks to seconds, divide by wall-clock delta and core count
                    cpuPercent = (cpuDelta / 10000000.0) / deltaSec * 100.0 / numCores;
                    if (cpuPercent > 100.0) cpuPercent = 100.0;
                }
                break;
            }
        }

        cpuInfos.push_back({cur.name, cur.pid, cpuPercent, cur.memory_bytes});
    }

    // Sort by descending CPU usage, then by memory as tiebreaker
    std::sort(cpuInfos.begin(), cpuInfos.end(), [](const ProcessCpuInfo& a, const ProcessCpuInfo& b) {
        if (a.cpu_percent != b.cpu_percent) return a.cpu_percent > b.cpu_percent;
        return a.memory_bytes > b.memory_bytes;
    });

    // Fill top-N process slots in shared memory
    uint32_t count = std::min((uint32_t)cpuInfos.size(), (uint32_t)SYSPULSE_MAX_PROCS);
    for (uint32_t i = 0; i < count; i++) {
        ProcessInfo& pi = vitals.top_procs[i];
        std::strncpy(pi.name, cpuInfos[i].name.c_str(), SYSPULSE_PROC_NAME_LEN - 1);
        pi.name[SYSPULSE_PROC_NAME_LEN - 1] = '\0';
        pi.pid = cpuInfos[i].pid;
        pi._pad0 = 0;
        pi.cpu_percent = cpuInfos[i].cpu_percent;
        pi.memory_bytes = cpuInfos[i].memory_bytes;
    }
    vitals.top_proc_count = count;

    // Store current snapshot for next delta calculation
    previousProcesses = std::move(currentProcesses);
    lastProcessSampleTime = currentMs;
}

// ============================================================
// Process Count
// ============================================================
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

// ============================================================
// System Uptime
// ============================================================
uint32_t HardwareProbe::querySystemUptime() {
    return static_cast<uint32_t>(GetTickCount64() / 1000);
}

// ============================================================
// Main Sampling Orchestrator
// ============================================================
void HardwareProbe::sample(SysPulseVitals& vitals) {
    auto now = std::chrono::system_clock::now();
    uint64_t currentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();

    lastSampleTimeMs = currentMs;

    vitals.magic = SYSPULSE_MAGIC;
    vitals.version = SYSPULSE_VERSION;
    vitals.timestamp_ms = currentMs;

    // CPU metrics (aggregate query also collects per-core data in same PDH call)
    vitals.cpu_usage_percent = queryCpuUtilization();
    queryPerCoreCpu(vitals);
    vitals.cpu_freq_mhz = queryActualCpuFrequency();
    vitals.cpu_temp_celsius = 0.0;  // Will be filled by WMI if available

    // Memory
    queryMemoryStatus(vitals);

    // GPU fields zeroed here — populated by Python gpu_probe.py via nvidia-smi
    vitals.gpu_usage_percent = 0.0;
    vitals.gpu_temp_celsius = 0.0;
    vitals.gpu_power_watts = 0.0;
    vitals.gpu_vram_total_bytes = 0;
    vitals.gpu_vram_used_bytes = 0;

    // I/O throughput
    queryDiskThroughput(vitals);
    queryNetworkThroughput(vitals);

    // System counters
    vitals.process_count = queryProcessCount();
    vitals.uptime_seconds = querySystemUptime();

    // Power / Battery
    queryBatteryStatus(vitals);

    // Per-drive storage
    queryDriveCapacity(vitals);

    // WMI thermal zone readings (fills cpu_temp_celsius if zones available)
    queryWmiTemperatures(vitals);

    // Top resource-consuming processes (most expensive query, runs last)
    queryTopProcesses(vitals);
}

// ============================================================
// Cleanup
// ============================================================
void HardwareProbe::shutdown() {
    if (cpuQuery) {
        PdhCloseQuery(cpuQuery);
        cpuQuery = nullptr;
        cpuTotalCounter = nullptr;
        std::memset(perCoreCounters, 0, sizeof(perCoreCounters));
        pdhInitialized = false;
    }
    if (diskQuery) {
        PdhCloseQuery(diskQuery);
        diskQuery = nullptr;
        diskReadCounter = nullptr;
        diskWriteCounter = nullptr;
        diskPdhInitialized = false;
    }
    if (netQuery) {
        PdhCloseQuery(netQuery);
        netQuery = nullptr;
        netRecvCounter = nullptr;
        netSentCounter = nullptr;
        netPdhInitialized = false;
    }
    shutdownWmi();
}
