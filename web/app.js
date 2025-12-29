// Implementation details:
// Frontend application orchestrator for the SysPulse monitoring dashboard.
// Dispatches asynchronous fetch queries to /api/vitals and /api/history at 1-second cadence.
// Computes dynamic thermal status classes (normal, warning, critical) and feeds the Canvas
// graphing pipeline with synchronized circular telemetry vectors.

let pollTimer = null;

function formatUptime(seconds) {
    const d = Math.floor(seconds / 86400);
    const h = Math.floor((seconds % 86400) / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);
    if (d > 0) {
        return `${d}d ${String(h).padStart(2, "0")}h ${String(m).padStart(2, "0")}m`;
    }
    return `${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}`;
}

function formatBytes(bytes) {
    if (!bytes || bytes <= 0) return "0.0 GB";
    const gb = bytes / (1024 * 1024 * 1024);
    return `${gb.toFixed(1)} GB`;
}

function updateThermalTag(tagElement, tempVal) {
    if (!tagElement) return;
    if (tempVal === null || tempVal === undefined || tempVal <= 0) {
        tagElement.textContent = "Offline";
        tagElement.className = "thermal-tag normal";
        return;
    }

    tagElement.textContent = `${tempVal.toFixed(1)} °C`;
    if (tempVal >= 85.0) {
        tagElement.className = "thermal-tag critical";
    } else if (tempVal >= 75.0) {
        tagElement.className = "thermal-tag warning";
    } else {
        tagElement.className = "thermal-tag normal";
    }
}

async function fetchTelemetrySnapshot() {
    try {
        const [vitalsRes, historyRes] = await Promise.all([
            fetch("/api/vitals", { cache: "no-store" }),
            fetch("/api/history", { cache: "no-store" })
        ]);

        if (!vitalsRes.ok || !historyRes.ok) return;

        const vitals = await vitalsRes.json();
        const history = await historyRes.json();

        updateDashboardUI(vitals, history);
        setConnectionState(true, vitals.source);
    } catch (err) {
        setConnectionState(false, null);
    }
}

function setConnectionState(online, source) {
    const dot = document.getElementById("live-dot");
    const badge = document.getElementById("source-badge");

    if (dot) {
        dot.className = online ? "pulse-indicator live" : "pulse-indicator offline";
    }
    if (badge) {
        if (!online) {
            badge.textContent = "Server Offline";
        } else if (source === "native_daemon") {
            badge.textContent = "C++ Shared Memory";
        } else {
            badge.textContent = "Kernel Fallback";
        }
    }
}

function updateDashboardUI(v, history) {
    // Header Info
    const uptimeEl = document.getElementById("uptime-val");
    if (uptimeEl && v.uptime_seconds !== undefined) {
        uptimeEl.textContent = formatUptime(v.uptime_seconds);
    }

    const procsEl = document.getElementById("procs-val");
    if (procsEl && v.process_count) {
        procsEl.textContent = v.process_count;
    }

    // CPU Card
    const cpuCircle = document.getElementById("cpu-gauge-circle");
    const cpuValEl = document.getElementById("cpu-val");
    const cpuFreqEl = document.getElementById("cpu-freq-val");
    const cpuTempEl = document.getElementById("cpu-temp-val");
    const cpuTempTag = document.getElementById("cpu-temp-tag");
    const cpuStateEl = document.getElementById("cpu-load-state");

    const cpuUsage = v.cpu_usage || 0.0;
    updateRadialGauge(cpuCircle, cpuUsage);
    if (cpuValEl) cpuValEl.textContent = cpuUsage.toFixed(1);
    if (cpuFreqEl && v.cpu_freq_mhz) cpuFreqEl.textContent = `${v.cpu_freq_mhz} MHz`;

    updateThermalTag(cpuTempTag, v.cpu_temp);
    if (cpuTempEl) {
        cpuTempEl.textContent = v.cpu_temp ? `${v.cpu_temp.toFixed(1)} °C` : "N/A";
    }

    if (cpuStateEl) {
        if (cpuUsage >= 80) cpuStateEl.textContent = "Heavy";
        else if (cpuUsage >= 40) cpuStateEl.textContent = "Active";
        else cpuStateEl.textContent = "Nominal";
    }

    // GPU Card
    const gpuCircle = document.getElementById("gpu-gauge-circle");
    const gpuValEl = document.getElementById("gpu-val");
    const gpuNameEl = document.getElementById("gpu-name-tag");
    const gpuTempEl = document.getElementById("gpu-temp-val");
    const gpuTempTag = document.getElementById("gpu-temp-tag");
    const vramTextEl = document.getElementById("vram-text");
    const gpuStateEl = document.getElementById("gpu-load-state");

    const gpuUsage = v.gpu_usage || 0.0;
    updateRadialGauge(gpuCircle, gpuUsage);
    if (gpuValEl) gpuValEl.textContent = gpuUsage.toFixed(1);
    if (gpuNameEl && v.gpu_name) gpuNameEl.textContent = v.gpu_name;

    updateThermalTag(gpuTempTag, v.gpu_temp);
    if (gpuTempEl) {
        gpuTempEl.textContent = v.gpu_temp ? `${v.gpu_temp.toFixed(1)} °C` : "N/A";
    }

    if (vramTextEl) {
        vramTextEl.textContent = `${formatBytes(v.vram_used_bytes)} / ${formatBytes(v.vram_total_bytes)}`;
    }

    if (gpuStateEl) {
        if (gpuUsage >= 70) gpuStateEl.textContent = "Rendering";
        else if (gpuUsage >= 20) gpuStateEl.textContent = "Video / Active";
        else gpuStateEl.textContent = "Idle";
    }

    // Memory & Capacity Bars
    const ramTextEl = document.getElementById("ram-usage-text");
    const ramBar = document.getElementById("ram-bar-fill");
    const ramTag = document.getElementById("ram-pct-tag");

    if (v.ram_total_bytes > 0) {
        const pct = (v.ram_used_bytes / v.ram_total_bytes) * 100.0;
        if (ramTextEl) ramTextEl.textContent = `${formatBytes(v.ram_used_bytes)} / ${formatBytes(v.ram_total_bytes)}`;
        if (ramBar) ramBar.style.width = `${pct.toFixed(1)}%`;
        if (ramTag) ramTag.textContent = `${pct.toFixed(1)}% Used`;
    }

    const vramBar = document.getElementById("vram-bar-fill");
    const vramUsageText = document.getElementById("vram-usage-text");
    if (v.vram_total_bytes > 0) {
        const vPct = (v.vram_used_bytes / v.vram_total_bytes) * 100.0;
        if (vramBar) vramBar.style.width = `${vPct.toFixed(1)}%`;
        if (vramUsageText) vramUsageText.textContent = `${formatBytes(v.vram_used_bytes)} / ${formatBytes(v.vram_total_bytes)}`;
    }

    // Render Canvas Time Series Graphs
    if (Array.isArray(history) && history.length > 0) {
        const utilCanvas = document.getElementById("utilization-canvas");
        const thermalCanvas = document.getElementById("thermal-canvas");

        renderTimeSeriesCanvas(utilCanvas, [
            {
                points: history.map(h => h.cpu),
                strokeColor: "#7dcfff",
                fillStart: "rgba(125, 207, 255, 0.18)"
            },
            {
                points: history.map(h => h.gpu),
                strokeColor: "#bb9af7",
                fillStart: "rgba(187, 154, 247, 0.12)"
            },
            {
                points: history.map(h => h.ram),
                strokeColor: "#7aa2f7",
                fillStart: "rgba(122, 162, 247, 0.08)"
            }
        ], 100, null);

        renderTimeSeriesCanvas(thermalCanvas, [
            {
                points: history.map(h => h.cpu_temp),
                strokeColor: "#e0af68",
                fillStart: "rgba(224, 175, 104, 0.15)"
            },
            {
                points: history.map(h => h.gpu_temp),
                strokeColor: "#f7768e",
                fillStart: "rgba(247, 118, 142, 0.15)"
            }
        ], 100, 80);
    }
}

// Fullscreen toggle support
function initFullscreenToggle() {
    const btn = document.getElementById("fullscreen-btn");
    if (!btn) return;

    btn.addEventListener("click", () => {
        if (!document.fullscreenElement) {
            document.documentElement.requestFullscreen().catch(() => {});
        } else {
            document.exitFullscreen().catch(() => {});
        }
    });
}

document.addEventListener("DOMContentLoaded", () => {
    initFullscreenToggle();
    fetchTelemetrySnapshot();
    pollTimer = setInterval(fetchTelemetrySnapshot, 1000);
});
