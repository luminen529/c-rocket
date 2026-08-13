"use strict";

const SIMULATION_SECONDS_PER_REAL_SECOND = 2;
const FAILURE_TIME = 39;
const EXPLOSION_DURATION = 1.4;

const state = {
    unsafe: [],
    safe: [],
    time: 0,
    playing: false,
    animationFrame: null,
    previousFrameTime: null,
    reducedMotion: window.matchMedia("(prefers-reduced-motion: reduce)").matches,
    xMax: 1,
    altitudeMax: 1
};

const elements = {
    time: document.getElementById("current-time"),
    playPause: document.getElementById("play-pause"),
    restart: document.getElementById("restart"),
    timeline: document.getElementById("timeline"),
    error: document.getElementById("load-error"),
    unsafe: makePanel("unsafe"),
    safe: makePanel("safe")
};

function makePanel(mode) {
    const svg = document.getElementById(`${mode}-flight`);

    return {
        svg,
        trajectory: svg.querySelector(".trajectory"),
        rocket: svg.querySelector(".rocket"),
        rocketArt: svg.querySelector(".rocket-art"),
        flame: svg.querySelector(".rocket-flame"),
        explosion: svg.querySelector(".explosion"),
        debris: Array.from(svg.querySelectorAll(".debris-pieces path")),
        wreckage: svg.querySelector(".wreckage"),
        status: document.getElementById(`${mode}-status`)
    };
}

function parseCsv(text) {
    const lines = text.trim().split(/\r?\n/);
    const headers = lines[0].split(",");

    return lines.slice(1).map((line) => {
        const values = line.split(",");
        const row = {};

        headers.forEach((header, index) => {
            row[header] = values[index];
        });

        return {
            mode: row.mode,
            time: Number(row.time),
            x: Number(row.x),
            altitude: Number(row.altitude),
            angle: Number(row.angle),
            status: row.status
        };
    });
}

async function loadCsv(path) {
    const response = await fetch(path);

    if (!response.ok) {
        throw new Error(`${path}: HTTP ${response.status}`);
    }

    return parseCsv(await response.text());
}

function pointFor(row) {
    const x = 30 + (row.x / state.xMax) * 440;
    const y = 460 - (row.altitude / state.altitudeMax) * 420;
    return { x, y };
}

function interpolateRow(rows, time) {
    const lowerIndex = Math.floor(time);
    const upperIndex = Math.min(Math.ceil(time), rows.length - 1);
    const progress = time - lowerIndex;
    const lower = rows[lowerIndex];
    const upper = rows[upperIndex];

    return {
        time,
        x: lower.x + (upper.x - lower.x) * progress,
        altitude: lower.altitude + (upper.altitude - lower.altitude) * progress,
        angle: lower.angle + (upper.angle - lower.angle) * progress,
        // Status changes are discrete events; do not reveal the next event early.
        status: lower.status
    };
}

function setStatus(element, status) {
    element.textContent = status;
    element.classList.toggle("control-lost", status === "CONTROL_LOST");
    element.classList.toggle("failed", status === "FAILED");
}

function renderFailureEffect(panel, currentPoint) {
    if (panel.explosion === null) {
        return;
    }

    const elapsed = state.time - FAILURE_TIME;
    const isExploding = elapsed >= 0 && elapsed < EXPLOSION_DURATION;
    const isWreckage = elapsed >= EXPLOSION_DURATION;

    panel.rocketArt.style.opacity = elapsed >= 0 ? "0" : "1";
    panel.explosion.style.opacity = isExploding ? "1" : "0";
    panel.wreckage.style.opacity = isWreckage ? "1" : "0";

    if (isExploding) {
        const progress = elapsed / EXPLOSION_DURATION;
        const burstScale = state.reducedMotion ? 1 : 0.35 + progress * 1.45;
        panel.explosion.setAttribute(
            "transform",
            `translate(${currentPoint.x.toFixed(2)} ${currentPoint.y.toFixed(2)}) ` +
            `scale(${burstScale.toFixed(2)})`
        );
        panel.explosion.style.opacity = String(Math.max(0, 1 - progress * 0.75));

        panel.debris.forEach((piece, index) => {
            const angle = (Math.PI * 2 * index) / panel.debris.length + 0.35;
            const distance = state.reducedMotion ? 22 : 12 + progress * 34;
            const x = Math.cos(angle) * distance;
            const y = Math.sin(angle) * distance + progress * 12;
            piece.setAttribute(
                "transform",
                `translate(${x.toFixed(2)} ${y.toFixed(2)}) ` +
                `rotate(${state.reducedMotion ? index * 45 : progress * 220 + index * 45})`
            );
        });
    }

    if (isWreckage) {
        panel.wreckage.setAttribute(
            "transform",
            `translate(${currentPoint.x.toFixed(2)} ${currentPoint.y.toFixed(2)}) ` +
            `rotate(${(state.time - FAILURE_TIME) * 42})`
        );
    }
}

function renderPanel(panel, rows) {
    const completedIndex = Math.floor(state.time);
    const visibleRows = rows.slice(0, completedIndex + 1);
    const current = interpolateRow(rows, state.time);
    const currentPoint = pointFor(current);
    const trajectoryRows = visibleRows.concat(
        state.time > completedIndex ? [current] : []
    );
    const points = trajectoryRows
        .map((row) => {
            const point = pointFor(row);
            return `${point.x.toFixed(2)},${point.y.toFixed(2)}`;
        })
        .join(" ");

    panel.trajectory.setAttribute("points", points);
    panel.rocket.setAttribute(
        "transform",
        `translate(${currentPoint.x.toFixed(2)} ${currentPoint.y.toFixed(2)}) ` +
        `rotate(${current.angle.toFixed(2)})`
    );
    const flameScale = state.reducedMotion ? 0.9 : 0.82 + Math.sin(state.time * 13) * 0.16;
    panel.flame.style.transform = `scaleY(${flameScale})`;
    renderFailureEffect(panel, currentPoint);
    setStatus(panel.status, current.status);
}

function render() {
    const current = interpolateRow(state.unsafe, state.time);

    renderPanel(elements.unsafe, state.unsafe);
    renderPanel(elements.safe, state.safe);
    elements.time.textContent = `T+${current.time.toFixed(1).padStart(4, "0")}s`;
    elements.timeline.value = String(state.time);
}

function stopPlayback() {
    state.playing = false;
    state.previousFrameTime = null;

    if (state.animationFrame !== null) {
        window.cancelAnimationFrame(state.animationFrame);
        state.animationFrame = null;
    }

    elements.playPause.textContent = "재생";
}

function animate(frameTime) {
    if (!state.playing) {
        return;
    }

    if (state.previousFrameTime === null) {
        state.previousFrameTime = frameTime;
    }

    const elapsedSeconds = (frameTime - state.previousFrameTime) / 1000;
    state.previousFrameTime = frameTime;
    state.time = Math.min(
        state.time + elapsedSeconds * SIMULATION_SECONDS_PER_REAL_SECOND,
        state.unsafe.length - 1
    );
    render();

    if (state.time >= state.unsafe.length - 1) {
        stopPlayback();
        return;
    }

    state.animationFrame = window.requestAnimationFrame(animate);
}

function startPlayback() {
    if (state.time >= state.unsafe.length - 1) {
        state.time = 0;
        render();
    }

    state.playing = true;
    elements.playPause.textContent = "일시정지";
    state.animationFrame = window.requestAnimationFrame(animate);
}

function bindControls() {
    elements.playPause.addEventListener("click", () => {
        if (!state.playing) {
            startPlayback();
        } else {
            stopPlayback();
        }
    });

    elements.restart.addEventListener("click", () => {
        stopPlayback();
        state.time = 0;
        render();
    });

    elements.timeline.addEventListener("input", (event) => {
        stopPlayback();
        state.time = Number(event.target.value);
        render();
    });
}

async function init() {
    try {
        [state.unsafe, state.safe] = await Promise.all([
            loadCsv("../output/unsafe.csv"),
            loadCsv("../output/safe.csv")
        ]);

        if (state.unsafe.length === 0 || state.unsafe.length !== state.safe.length) {
            throw new Error("두 CSV의 데이터 길이가 올바르지 않습니다.");
        }

        const allRows = state.unsafe.concat(state.safe);
        state.xMax = Math.max(...allRows.map((row) => row.x), 1);
        state.altitudeMax = Math.max(...allRows.map((row) => row.altitude), 1);
        elements.timeline.max = String(state.unsafe.length - 1);
        elements.timeline.step = "0.1";

        bindControls();
        render();
    } catch (error) {
        elements.error.hidden = false;
        elements.error.textContent = `데이터 로드 실패: ${error.message}`;
        elements.playPause.disabled = true;
        elements.restart.disabled = true;
        elements.timeline.disabled = true;
        console.error(error);
    }
}

init();
