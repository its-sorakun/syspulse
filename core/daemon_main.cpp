// Implementation details:
// Background native daemon entrypoint for Windows system telemetry.
// Allocates page-file-backed named shared memory (Local\SysPulse_Vitals) sized
// for the v2 1536-byte telemetry struct. Periodically polls all hardware subsystems
// via HardwareProbe (CPU per-core, disk/net I/O, drives, battery, WMI thermals,
// top-N processes) and commits snapshots at 1-second cadence.
// Intercepts console control signals for graceful teardown of PDH queries,
// WMI COM connections, and kernel shared memory handles.

#include "include/shared_memory.h"
#include "include/hardware_probe.h"
#include <iostream>
#include <windows.h>
#include <csignal>
#include <psapi.h>

static volatile bool g_running = true;

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_running = false;
        return TRUE;
    default:
        return FALSE;
    }
}

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    std::cout << "[SysPulse Core v2] Initializing hardware probe daemon..." << std::endl;
    std::cout << "[SysPulse Core v2] Shared memory payload size: "
              << sizeof(SysPulseVitals) << " bytes" << std::endl;

    // Allocate v2 struct (1536 bytes) in Windows paging file mapped to named shared memory
    HANDLE hMapFile = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(SysPulseVitals),
        SYSPULSE_SHM_NAME
    );

    if (hMapFile == NULL) {
        std::cerr << "[SysPulse Core Error] Failed to create shared memory block. Win32 code: "
                  << GetLastError() << std::endl;
        return 1;
    }

    SysPulseVitals* pVitals = static_cast<SysPulseVitals*>(
        MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SysPulseVitals))
    );

    if (pVitals == nullptr) {
        std::cerr << "[SysPulse Core Error] Failed to map view of shared memory. Win32 code: "
                  << GetLastError() << std::endl;
        CloseHandle(hMapFile);
        return 1;
    }

    // Zero-initialize entire v2 shared memory buffer
    ZeroMemory(pVitals, sizeof(SysPulseVitals));
    pVitals->magic = SYSPULSE_MAGIC;
    pVitals->version = SYSPULSE_VERSION;

    HardwareProbe probe;
    if (!probe.initialize()) {
        std::cerr << "[SysPulse Core Warning] Hardware probe initialization partially degraded." << std::endl;
        std::cerr << "[SysPulse Core Warning] WMI thermal zones may require administrator elevation." << std::endl;
    }

    std::cout << "[SysPulse Core v2] Telemetry features:" << std::endl;
    std::cout << "  - CPU: aggregate + per-core utilization (PDH)" << std::endl;
    std::cout << "  - CPU: actual core frequency (CallNtPowerInformation)" << std::endl;
    std::cout << "  - CPU: thermal zone temperature (WMI ACPI)" << std::endl;
    std::cout << "  - Memory: physical RAM usage (GlobalMemoryStatusEx)" << std::endl;
    std::cout << "  - Disk: read/write throughput (PDH PhysicalDisk)" << std::endl;
    std::cout << "  - Network: recv/sent throughput (PDH Network Interface)" << std::endl;
    std::cout << "  - Storage: per-drive capacity (GetDiskFreeSpaceExW)" << std::endl;
    std::cout << "  - Power: battery status (GetSystemPowerStatus)" << std::endl;
    std::cout << "  - Processes: top-10 CPU/memory consumers (Toolhelp32)" << std::endl;

    std::cout << "[SysPulse Core v2] Shared memory ready at: Local\\SysPulse_Vitals" << std::endl;
    std::cout << "[SysPulse Core v2] Entering 1000ms polling loop. Press Ctrl+C to stop." << std::endl;

    while (g_running) {
        probe.sample(*pVitals);
        Sleep(1000);
    }

    std::cout << "[SysPulse Core v2] Shutdown signal received. Releasing resources..." << std::endl;

    probe.shutdown();
    UnmapViewOfFile(pVitals);
    CloseHandle(hMapFile);

    std::cout << "[SysPulse Core v2] Daemon exited cleanly." << std::endl;
    return 0;
}
