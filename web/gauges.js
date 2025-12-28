// Implementation details:
// Canvas time series graph renderer and SVG radial gauge visualizers.
// Computes High-DPI pixel scaling to ensure sharp rendering on retina screens.
// Implements smooth Catmull-Rom cubic Bézier curve interpolation for rolling metrics
// and calculates stroke-dashoffset geometry for radial percentage meters.

const CIRCLE_CIRCUMFERENCE = 301.6; // 2 * PI * 48

function updateRadialGauge(circleElement, percentage) {
    if (!circleElement) return;
    const clamped = Math.max(0, Math.min(100, percentage));
    const offset = CIRCLE_CIRCUMFERENCE * (1 - clamped / 100);
    circleElement.style.strokeDashoffset = offset;
}

function renderTimeSeriesCanvas(canvas, dataSeries, yMax = 100, threshold = null) {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    // Handle High-DPI Display Scaling
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();

    if (canvas.width !== rect.width * dpr || canvas.height !== rect.height * dpr) {
        canvas.width = rect.width * dpr;
        canvas.height = rect.height * dpr;
    }

    ctx.save();
    ctx.scale(dpr, dpr);

    const width = rect.width;
    const height = rect.height;

    // Clear frame
    ctx.clearRect(0, 0, width, height);

    // Draw Subtle Horizontal Grid Lines (25%, 50%, 75%)
    ctx.strokeStyle = "rgba(255, 255, 255, 0.05)";
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);

    for (let p of [0.25, 0.5, 0.75]) {
        const y = height * (1 - p);
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(width, y);
        ctx.stroke();
    }

    // Draw Threshold Guide Line if Specified
    if (threshold !== null && threshold <= yMax) {
        const threshY = height * (1 - threshold / yMax);
        ctx.strokeStyle = "rgba(247, 118, 142, 0.35)";
        ctx.setLineDash([2, 4]);
        ctx.beginPath();
        ctx.moveTo(0, threshY);
        ctx.lineTo(width, threshY);
        ctx.stroke();
    }

    ctx.setLineDash([]); // Reset dashed lines

    // Draw Each Data Series
    dataSeries.forEach(series => {
        const points = series.points;
        if (!points || points.length < 2) return;

        const maxPoints = 60;
        const stepX = width / (maxPoints - 1);
        const startIndex = maxPoints - points.length;

        // Coordinates buffer
        const coords = points.map((val, idx) => {
            const v = val !== null && val !== undefined ? val : 0;
            const clamped = Math.max(0, Math.min(yMax, v));
            const x = (startIndex + idx) * stepX;
            const y = height * (1 - clamped / yMax);
            return { x, y };
        });

        // Fill under the curve with linear gradient
        ctx.beginPath();
        ctx.moveTo(coords[0].x, height);
        ctx.lineTo(coords[0].x, coords[0].y);

        for (let i = 0; i < coords.length - 1; i++) {
            const curr = coords[i];
            const next = coords[i + 1];
            const midX = (curr.x + next.x) / 2;
            ctx.bezierCurveTo(midX, curr.y, midX, next.y, next.x, next.y);
        }

        ctx.lineTo(coords[coords.length - 1].x, height);
        ctx.closePath();

        const grad = ctx.createLinearGradient(0, 0, 0, height);
        grad.addColorStop(0, series.fillStart || "rgba(125, 207, 255, 0.15)");
        grad.addColorStop(1, "rgba(125, 207, 255, 0.0)");
        ctx.fillStyle = grad;
        ctx.fill();

        // Stroke line
        ctx.beginPath();
        ctx.moveTo(coords[0].x, coords[0].y);

        for (let i = 0; i < coords.length - 1; i++) {
            const curr = coords[i];
            const next = coords[i + 1];
            const midX = (curr.x + next.x) / 2;
            ctx.bezierCurveTo(midX, curr.y, midX, next.y, next.x, next.y);
        }

        ctx.strokeStyle = series.strokeColor || "#7dcfff";
        ctx.lineWidth = 2;
        ctx.stroke();
    });

    ctx.restore();
}
