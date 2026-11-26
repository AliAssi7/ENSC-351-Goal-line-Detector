const socket = io();

const simulateBtn = document.getElementById("simulateBtn");
const confirmBtn = document.getElementById("confirmBtn");
const rejectBtn = document.getElementById("rejectBtn");

const reviewStatus = document.getElementById("reviewStatus");
const decisionTitle = document.getElementById("decisionTitle");
const decisionDescription = document.getElementById("decisionDescription");
const eventTag = document.getElementById("eventTag");
const eventHistory = document.getElementById("eventHistory");
const goalCount = document.getElementById("goalCount");
const replayStatus = document.getElementById("replayStatus");
const systemStatus = document.querySelector(".system-status");

const irStatus = document.getElementById("irStatus");
const cameraStatus = document.getElementById("cameraStatus");
const cameraTag = document.getElementById("cameraTag");
const cameraCanvas = document.getElementById("cameraCanvas");
const cameraStream = document.getElementById("cameraStream");
const cameraPlaceholder = document.getElementById("cameraPlaceholder");
const cameraMessage = document.getElementById("cameraMessage");
const cameraInfo = document.getElementById("cameraInfo");
const calibrateBtn = document.getElementById("calibrateBtn");

const replayControls = document.getElementById("replayControls");
const prevBtn = document.getElementById("prevBtn");
const playBtn = document.getElementById("playBtn");
const nextBtn = document.getElementById("nextBtn");
const replaySlider = document.getElementById("replaySlider");
const replayLabel = document.getElementById("replayLabel");

const context = cameraCanvas.getContext("2d");

const SOURCE_LABELS = {
    camera: "Camera: ball wholly over the line",
    ir: "IR: beam broken",
    manual: "Manual review"
};

const CAMERA_LABELS = {
    yolo: "YOLO Online", starting: "Starting", live: "Live", sim: "Simulated", file: "Video File", offline: "Offline"
};

// The YOLO camera only supports the review; it cannot show the whole ball crossed.
const CAMERA_EVIDENCE = {
    pending: "Camera (YOLO): checking for the ball",
    seen: "Camera (YOLO): ball detected around the trigger",
    missed: "Camera (YOLO): no ball detected around the trigger",
    ignored: "Camera (YOLO): trigger ignored during the camera's 3 s cooldown",
    unavailable: "Camera (YOLO): detection result unavailable"
};
const IR_LABELS = { ok: "Online", error: "Error", disabled: "Disabled", offline: "Offline" };
const OK_CAMERA = ["yolo", "live", "sim", "file"];
const CAMERA_EVIDENCE_STATUS = { seen: "ok", missed: "bad", ignored: "bad", unavailable: "bad" };

let state = null;
let pending = null;
let liveFrame = null;
let replayFrame = null;
let calibration = null;
let sliding = false;
let streamFailed = false;
let drawSequence = 0;

simulateBtn.disabled = true;
confirmBtn.disabled = true;
rejectBtn.disabled = true;

socket.on("connect", render);

socket.on("disconnect", () => {
    liveFrame = null;
    calibration = null;
    render();
});

socket.on("update", (update) => {
    state = update;
    pending = state.pending;

    if (!pending || (replayFrame && replayFrame.id !== pending.id)) {
        replayFrame = null;
    }

    render();
});

socket.on("live", (frame) => {
    liveFrame = frame;
    drawCamera();
});

socket.on("replayFrame", (frame) => {
    if (pending && frame.id === pending.id) {
        replayFrame = frame;
        drawCamera();
    }
});

function sourcesText(sources) {
    return sources.map((s) => s === "ir" ? "IR" : s[0].toUpperCase() + s.slice(1)).join(" + ");
}

function render() {
    const connected = socket.connected && state !== null;
    const board = connected ? state.board : { connected: false };

    if (!socket.connected) {
        setStatus(systemStatus, "Server Disconnected", "bad");
    } else if (!board.connected) {
        setStatus(systemStatus, "Demo Mode - BeagleY-AI Disconnected", "bad");
    } else {
        const modes = { monitoring: "Monitoring", review: "Reviewing", goal: "Goal Confirmed" };
        setStatus(systemStatus, `BeagleY-AI Connected - ${modes[board.state] || "Starting"}`, "ok");
    }

    if (!board.connected) {
        setStatus(irStatus, "Disconnected", "bad");
        setStatus(cameraStatus, "Disconnected", "bad");
    } else {
        setStatus(irStatus, IR_LABELS[board.ir] || "Starting",
            board.ir === "ok" ? "ok" : ["error", "offline"].includes(board.ir) ? "bad" : null);
        setStatus(cameraStatus, CAMERA_LABELS[board.camera] || "Starting",
            OK_CAMERA.includes(board.camera) ? "ok" : board.camera === "offline" ? "bad" : null);
    }

    simulateBtn.disabled = !connected || pending !== null;
    confirmBtn.disabled = !connected || pending === null;
    rejectBtn.disabled = !connected || pending === null;

    if (connected) {
        goalCount.textContent = state.goals;
        renderReview();
        showHistory(state.history);
    }

    renderReplay(board);
    drawCamera();
}

// status: "ok" (green), "bad" (red) or null (neutral).
function setStatus(element, text, status) {
    element.textContent = text;
    element.classList.toggle("ok", status === "ok");
    element.classList.toggle("bad", status === "bad");
}

function setDescription(parts) {
    decisionDescription.textContent = "";

    parts.forEach(([text, status]) => {
        const span = document.createElement("span");
        span.textContent = text + " ";
        if (status) {
            span.className = status;
        }
        decisionDescription.appendChild(span);
    });
}

function renderReview() {
    if (pending) {
        const agree = pending.sources.includes("camera") && pending.sources.includes("ir");
        const parts = pending.sources.map((s) => [SOURCE_LABELS[s] + ".", s === "camera" ? "ok" : null]);

        if (pending.camera) {
            parts.push([CAMERA_EVIDENCE[pending.camera] + ".", CAMERA_EVIDENCE_STATUS[pending.camera] || null]);
        }

        parts.push([(agree ? "Camera and IR agree. " : "") +
            (pending.camera ? "The camera is supporting evidence only. " : "") +
            "Review the footage before deciding." +
            ` Detected at ${new Date(pending.time).toLocaleTimeString()}.`, null]);

        setStatus(reviewStatus, "Pending", null);
        setStatus(decisionTitle, `Possible Goal - Event #${pending.number}`, null);
        setDescription(parts);
        eventTag.textContent = "Review Required";
        return;
    }

    setStatus(reviewStatus, "Ready", "ok");
    replayStatus.textContent = "";

    if (state.history.length > 0) {
        const last = state.history[0];
        const goal = last.decision === "GOAL";

        setStatus(decisionTitle, goal ? "Goal Confirmed" : "No Goal", goal ? "ok" : "bad");
        decisionDescription.textContent =
            `Event #${last.number} reviewed at ${new Date(last.time).toLocaleTimeString()}.`;
        eventTag.textContent = "Reviewed";
    } else {
        setStatus(decisionTitle, "No Pending Review", null);
        decisionDescription.textContent = "Waiting for a goal-line event.";
        eventTag.textContent = "Waiting";
    }
}

function renderReplay(board) {
    const replay = state && state.replay;
    const canControl = socket.connected && board.connected && pending && !pending.local;

    replayControls.hidden = !pending;

    if (!pending) {
        return;
    }

    if (!replay || replay.frames === 0) {
        replayLabel.textContent = pending.local ? "Demo event - no footage" : "No replay footage";
        replayStatus.textContent = "";
        replaySlider.max = 0;
        replaySlider.value = 0;
        [prevBtn, playBtn, nextBtn, replaySlider].forEach((el) => { el.disabled = true; });
        return;
    }

    replayLabel.textContent = `Frame ${replay.frame + 1} / ${replay.frames}` +
        (replay.complete ? "" : " (recording)");
    replayStatus.textContent = `Replay ${replay.playing ? "playing" : "paused"} - ` +
        `use the dashboard or the rotary encoder to step through frames.`;
    playBtn.textContent = replay.playing ? "Pause" : "Play";

    replaySlider.max = replay.frames - 1;
    if (!sliding) {
        replaySlider.value = replay.frame;
    }

    prevBtn.disabled = !canControl || replay.frame === 0;
    nextBtn.disabled = !canControl || replay.frame >= replay.frames - 1;
    playBtn.disabled = !canControl;
    replaySlider.disabled = !canControl;
}

function replayMessage() {
    if (pending.local) {
        return "Demo event - no camera footage";
    }

    if (pending.cameraReplay === "pending") {
        return "Waiting for the camera replay...";
    }

    if (pending.cameraReplay === "unavailable") {
        return pending.camera === "ignored"
            ? "Replay unavailable: the camera ignored this trigger"
            : "Replay unavailable for this event";
    }

    return pending.cameraReplay === "ready" ? "Loading replay..." : "No replay footage for this event";
}

function showStream(url) {
    if (url && cameraStream.dataset.src !== url) {
        cameraStream.dataset.src = url;
        cameraStream.src = url;
    } else if (!url && cameraStream.dataset.src) {
        // Closing the connection stops goal_cam.py encoding frames for us.
        cameraStream.removeAttribute("src");
        delete cameraStream.dataset.src;
    }

    cameraStream.hidden = !url;
}

cameraStream.addEventListener("error", () => {
    if (!cameraStream.dataset.src) {
        return;
    }

    streamFailed = true;
    showStream(null);
    drawCamera();

    setTimeout(() => {
        streamFailed = false;
        drawCamera();
    }, 3000);
});

function drawCamera() {
    const board = state && socket.connected ? state.board : { connected: false };
    let frame = null;
    let stream = null;
    let message = "No Camera Signal";
    let tag = "Offline";

    if (!socket.connected) {
        message = "Server Disconnected";
    } else if (pending) {
        tag = "Recorded Replay";
        frame = replayFrame;
        message = replayMessage();
    } else if (!board.connected) {
        message = "BeagleY-AI Disconnected";
    } else if (board.stream) {
        if (board.camera === "yolo" && !streamFailed) {
            // goal_cam.py's stream also shows its own slow-motion replay after a trigger.
            tag = "Camera Stream";
            stream = board.stream;
        } else {
            message = board.camera === "starting" ? "Camera starting..." :
                board.camera === "yolo" ? "Camera stream unavailable, retrying" : "Camera Offline";
        }
    } else if (board.camera && !["offline", "starting"].includes(board.camera)) {
        tag = CAMERA_LABELS[board.camera];
        frame = liveFrame;
        message = "Waiting for video";
    } else {
        message = "Camera Offline";
    }

    showStream(stream);
    cameraTag.textContent = tag;
    cameraCanvas.hidden = !frame;
    cameraPlaceholder.hidden = !!(frame || stream);
    cameraMessage.textContent = message;
    calibrateBtn.hidden = !!board.stream;
    calibrateBtn.disabled = !(frame && frame === liveFrame);

    if (calibrateBtn.disabled) {
        calibration = null;
    }

    calibrateBtn.textContent = calibration ? "Cancel" : "Calibrate Line";
    cameraCanvas.classList.toggle("calibrating", calibration !== null);

    if (calibration) {
        const steps = ["first point on the goal-side edge of the line",
                       "second point on the line", "any point inside the goal"];
        cameraInfo.textContent = `Click the ${steps[calibration.length]}`;
    } else if (frame) {
        cameraInfo.textContent = frame === replayFrame
            ? `Recorded replay - frame ${frame.index + 1}`
            : `BeagleY-AI - ${frame.width}x${frame.height}`;
    } else if (stream && board.cameraStats) {
        const stats = board.cameraStats;
        cameraInfo.textContent =
            `Camera stream - ${stats.fps} fps, model ${stats.modelMs} ms, ball: ${stats.ball ? "yes" : "no"}`;
    } else {
        cameraInfo.textContent = "BeagleY-AI";
    }

    if (frame) {
        drawFrame(frame);
    }
}

function resizeCanvas(width, height) {
    if (cameraCanvas.width !== width || cameraCanvas.height !== height) {
        cameraCanvas.width = width;
        cameraCanvas.height = height;
    }
}

function drawFrame(frame) {
    const sequence = ++drawSequence;

    if (frame.format === "jpeg") {
        const image = new Image();

        image.onload = () => {
            // Decoding is asynchronous; skip frames that were already replaced.
            if (sequence === drawSequence) {
                resizeCanvas(image.naturalWidth, image.naturalHeight);
                context.drawImage(image, 0, 0);
                drawOverlay(frame);
            }
        };

        image.src = "data:image/jpeg;base64," + frame.data;
        return;
    }

    resizeCanvas(frame.width, frame.height);

    const bytes = atob(frame.data);
    const image = context.createImageData(frame.width, frame.height);

    for (let i = 0, j = 0; i < bytes.length; i += 3, j += 4) {
        image.data[j] = bytes.charCodeAt(i);
        image.data[j + 1] = bytes.charCodeAt(i + 1);
        image.data[j + 2] = bytes.charCodeAt(i + 2);
        image.data[j + 3] = 255;
    }

    context.putImageData(image, 0, 0);
    drawOverlay(frame);
}

function drawOverlay(frame) {
    const line = state && state.line;
    context.lineWidth = 1;

    if (line && !calibration) {
        context.strokeStyle = "darkorange";
        context.beginPath();
        context.moveTo(line.x1 + 0.5, line.y1 + 0.5);
        context.lineTo(line.x2 + 0.5, line.y2 + 0.5);
        context.stroke();
    }

    if (frame.ball) {
        // Wholly over the line (zone 3) gets a thicker ring.
        context.strokeStyle = "limegreen";
        context.lineWidth = frame.ball.zone === 3 ? 3 : 1;
        context.beginPath();
        context.arc(frame.ball.x, frame.ball.y, frame.ball.r + 3, 0, 2 * Math.PI);
        context.stroke();
    }

    (calibration || []).forEach((p) => {
        context.fillStyle = "royalblue";
        context.fillRect(p.x - 2, p.y - 2, 5, 5);
    });
}

function showHistory(history) {
    eventHistory.textContent = "";

    if (history.length === 0) {
        eventHistory.textContent = "No events recorded yet.";
        return;
    }

    history.forEach((event) => {
        const entry = document.createElement("div");
        entry.className = "history-entry";

        const label = document.createElement("span");
        const decision = document.createElement("span");
        decision.textContent = event.decision;
        decision.className = event.decision === "GOAL" ? "ok" : "bad";
        label.append(`Event #${event.number} - `, decision, ` (${sourcesText(event.sources)}`);

        if (event.camera && event.camera !== "pending") {
            const camera = document.createElement("span");
            camera.textContent = { seen: "camera: ball seen", missed: "camera: no ball" }[event.camera] ||
                "camera: no result";
            camera.className = CAMERA_EVIDENCE_STATUS[event.camera];
            label.append("; ", camera);
        }

        label.append(")");

        const time = document.createElement("span");
        time.textContent = new Date(event.time).toLocaleTimeString();

        entry.append(label, time);
        eventHistory.appendChild(entry);
    });
}

simulateBtn.addEventListener("click", () => {
    if (socket.connected && !pending) {
        socket.emit("simulateGoal");
    }
});

confirmBtn.addEventListener("click", () => {
    if (socket.connected && pending) {
        socket.emit("decision", {
            id: pending.id,
            result: "GOAL"
        });
    }
});

rejectBtn.addEventListener("click", () => {
    if (socket.connected && pending) {
        socket.emit("decision", {
            id: pending.id,
            result: "NO GOAL"
        });
    }
});

prevBtn.addEventListener("click", () => socket.emit("replay", { action: "prev" }));
nextBtn.addEventListener("click", () => socket.emit("replay", { action: "next" }));

playBtn.addEventListener("click", () => {
    const playing = state && state.replay && state.replay.playing;
    socket.emit("replay", { action: playing ? "pause" : "play" });
});

// A click that does not move the thumb fires no change event, so stop on any release.
replaySlider.addEventListener("pointerdown", () => { sliding = true; });
window.addEventListener("pointerup", () => { sliding = false; });
window.addEventListener("pointercancel", () => { sliding = false; });

replaySlider.addEventListener("input", () => {
    socket.emit("replay", { action: "seek", frame: Number(replaySlider.value) });
});

calibrateBtn.addEventListener("click", () => {
    calibration = calibration ? null : [];
    drawCamera();
});

cameraCanvas.addEventListener("click", (event) => {
    if (!calibration) {
        return;
    }

    // Map the click through the canvas's object-fit: contain scaling.
    const rect = cameraCanvas.getBoundingClientRect();
    const scale = Math.min(rect.width / cameraCanvas.width, rect.height / cameraCanvas.height);
    const x = Math.round((event.clientX - rect.left - (rect.width - cameraCanvas.width * scale) / 2) / scale);
    const y = Math.round((event.clientY - rect.top - (rect.height - cameraCanvas.height * scale) / 2) / scale);

    if (x < 0 || y < 0 || x >= cameraCanvas.width || y >= cameraCanvas.height) {
        return;
    }

    calibration.push({ x, y });

    if (calibration.length === 3) {
        const [a, b, goal] = calibration;
        socket.emit("calibrate", { x1: a.x, y1: a.y, x2: b.x, y2: b.y, gx: goal.x, gy: goal.y });
        calibration = null;
    }

    drawCamera();
});
