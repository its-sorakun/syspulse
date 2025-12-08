// Implementation details:
// Background native daemon entrypoint for Windows system telemetry.
// Allocates page-file-backed named shared memory (Local\SysPulse_Vitals),
// periodically polls hardware counters via HardwareProbe, and commits
// telemetry snapshots at 1-second cadence. Intercepts OS signals for graceful teardown.

#include "include/shared_memory.h"
#include "include/hardware_probe.h"
#include <iostream>
#include <windows.h>
#include <csignal>

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

    std::cout << "[SysPulse Core] Initializing hardware probe daemon..." << std::endl;

    // Allocate 128 bytes in Windows paging file mapped to named shared memory
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

    // Zero-initialize shared memory buffer
    ZeroMemory(pVitals, sizeof(SysPulseVitals));
    pVitals->magic = SYSPULSE_MAGIC;
    pVitals->version = SYSPULSE_VERSION;

    HardwareProbe probe;
    if (!probe.initialize()) {
        std::cerr << "[SysPulse Core Warning] Hardware probe initialization partially degraded." << std::endl;
    }

    std::cout << "[SysPulse Core] Telemetry shared memory ready at: " << "Local\\SysPulse_Vitals" << std::endl;
    std::cout << "[SysPulse Core] Entering 1000ms polling loop. Press Ctrl+C to stop." << std::endl;

    while (g_running) {
        probe.sample(*pVitals);
        Sleep(1000);
    }

    std::cout << "[SysPulse Core] Shutdown signal received. Unmapping shared memory..." << std::endl;

    probe.shutdown();
    UnmapViewOfFile(pVitals);
    CloseHandle(hMapFile);

    std::cout << "[SysPulse Core] Daemon exited cleanly." << std::endl;
    return 0;
}
