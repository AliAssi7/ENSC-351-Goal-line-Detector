const socket = io();

const simulateBtn = document.getElementById("simulateBtn");
const confirmBtn = document.getElementById("confirmBtn");
const rejectBtn = document.getElementById("rejectBtn");
const autoActions = document.getElementById("autoActions");
const viewBtn = document.getElementById("viewBtn");
const correctBtn = document.getElementById("correctBtn");

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

const CAMERA_LABELS = {
    yolo: "YOLO Online", starting: "Starting", live: "Live", sim: "Simulated", file: "Video File", offline: "Offline"
};

// [label, colour class] for each board state.
const STATE_LABELS = {
    monitoring: ["MONITORING", null],
    evaluating: ["EVALUATING EVIDENCE", null],
    awaiting: ["AWAITING DECISION", "wait"],
    goal: ["GOAL", "ok"],
    nogoal: ["NO GOAL", "bad"]
};

const AWAIT_REASONS = {
    manual: "Review initiated by the referee using Simulate Event.",
    no_ball: "IR interruption detected, but the camera did not detect a soccer ball.",
    low_confidence: "IR interruption detected, but the camera could not confidently verify a soccer ball.",
    ball_not_moving: "IR interruption detected, but the ball the camera saw was not moving, so it may not be the one at the goal.",
    insufficient: "IR interruption detected, but there was not enough camera footage to verify it.",
    unverified: "IR interruption detected. The camera reported a ball without a confidence score.",
    camera_busy: "IR interruption detected, but the camera was still saving the previous replay.",
    camera_unavailable: "IR interruption detected, but the camera is unavailable.",
    timeout: "IR interruption detected, but the camera did not answer in time."
};

// The camera supports the decision; it cannot show the whole ball crossed.
const CAMERA_EVIDENCE = {
    pending: ["checking the footage", null],
    confident: ["soccer ball confidently detected", "ok"],
    stationary: ["ball detected but not moving", "bad"],
    low: ["ball detected at low confidence", "bad"],
    none: ["no soccer ball detected", "bad"],
    insufficient: ["not enough footage", "bad"],
    seen: ["ball reported, no confidence score", null],
    missed: ["no ball reported", "bad"],
    ignored: ["trigger ignored (camera busy)", "bad"],
    unavailable: ["no result", "bad"]
};
const IR_LABELS = { ok: "Online", degraded: "Degraded", error: "Error", disabled: "Disabled", offline: "Offline" };
const OK_CAMERA = ["yolo", "live", "sim", "file"];

let state = null;
let pending = null;
let viewing = null;     // the automatic goal whose replay is shown
let replayEvent = null; // pending or viewing: the event in the replay panel
let liveFrame = null;
let replayFrame = null;
let calibration = null;
let sliding = false;
let streamFailed = false;
let streamAttempt = 0;
let drawSequence = 0;
let decidedId = null;   // stops a double click sending two decisions

simulateBtn.disabled = true;
confirmBtn.disabled = true;
rejectBtn.disabled = true;

socket.on("connect", () => {
    decidedId = null;
    render();
});

socket.on("disconnect", () => {
    liveFrame = null;
    calibration = null;
    render();
});

socket.on("update", (update) => {
    state = update;
    pending = state.pending;
    viewing = state.viewing ? state.history.find((e) => e.id === state.viewing) || null : null;
    replayEvent = pending || viewing;

    if (!replayEvent || (replayFrame && replayFrame.id !== replayEvent.id)) {
        replayFrame = null;
    }

    render();
});

socket.on("live", (frame) => {
    liveFrame = frame;
    drawCamera();
});

socket.on("replayFrame", (frame) => {
    if (replayEvent && frame.id === replayEvent.id) {
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
        setStatus(systemStatus, "BeagleY-AI Disconnected", "bad");
    } else {
        const mode = STATE_LABELS[board.state] ? STATE_LABELS[board.state][0].toLowerCase() : "starting";
        setStatus(systemStatus, `BeagleY-AI Connected - ${mode}`, "ok");
    }

    if (!board.connected) {
        setStatus(irStatus, "Disconnected", "bad");
        setStatus(cameraStatus, "Disconnected", "bad");
    } else {
        setStatus(irStatus, IR_LABELS[board.ir] || "Starting",
            board.ir === "ok" ? "ok" : ["degraded", "error", "offline"].includes(board.ir) ? "bad" : null);
        setStatus(cameraStatus, CAMERA_LABELS[board.camera] || "Starting",
            OK_CAMERA.includes(board.camera) ? "ok" : board.camera === "offline" ? "bad" : null);
    }

    // Decisions only once the event is waiting for the referee.
    const awaiting = pending !== null && pending.phase === "awaiting" && pending.id !== decidedId;

    simulateBtn.disabled = !connected || !board.connected || pending !== null;
    confirmBtn.disabled = !connected || !awaiting;
    rejectBtn.disabled = !connected || !awaiting;

    // The latest automatic goal can be replayed and corrected.
    const auto = connected ? latestAutoGoal() : null;
    autoActions.hidden = !auto || pending !== null;
    viewBtn.textContent = viewing ? "Back to Live" : "Review Replay";
    viewBtn.disabled = !board.connected || !auto || (!viewing && state.history[0] !== auto);
    correctBtn.disabled = !board.connected || !auto;

    if (connected) {
        goalCount.textContent = state.goals;
        const phase = board.connected ? board.state : pending && pending.phase;
        const [label, colour] = STATE_LABELS[phase] || ["MONITORING", null];
        setStatus(reviewStatus, label, colour);
        renderReview();
        showHistory(state.history);
    }

    renderReplay(board);
    drawCamera();
}

function latestAutoGoal() {
    const entry = state.history.find((e) => e.auto);
    return entry && entry.decision === "GOAL" && !entry.corrected ? entry : null;
}

// status: "ok" (green), "wait" (yellow), "bad" (red) or null (neutral).
function setStatus(element, text, status) {
    element.textContent = text;
    element.classList.toggle("ok", status === "ok");
    element.classList.toggle("wait", status === "wait");
    element.classList.toggle("bad", status === "bad");
}

function confidenceText(c) {
    if (!c) {
        return "";
    }
    let text = `best ${c.best}%, ball in ${c.ballRuns} of ${c.runs} model runs`;
    if (c.best > 0) {
        text += `, ${c.firstMs} to ${c.lastMs} ms from the beam break`;
    }
    return text;
}

function cameraParts(event) {
    if (!event.camera) {
        return [];
    }
    const [text, colour] = CAMERA_EVIDENCE[event.camera];
    const details = confidenceText(event.confidence);
    return [[`Camera (YOLO): ${text}${details ? ` (${details})` : ""}.`, colour]];
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
    replayStatus.textContent = "";

    if (pending) {
        const evaluating = pending.phase === "evaluating";
        const title = evaluating ? "Evaluating Evidence" : pending.manual ? "Manual Review" : "Awaiting Decision";
        const reason = evaluating
            ? "IR interruption detected. Checking the camera footage for a soccer ball..."
            : AWAIT_REASONS[pending.reason] || "Waiting for the referee's decision.";

        setStatus(decisionTitle, `${title} - Event #${pending.number}`, evaluating ? null : "wait");
        setDescription([
            [reason, null],
            [pending.manual ? "Manual event." : `Automatic event, triggered by ${sourcesText(pending.sources)}.`, null],
            ...cameraParts(pending),
            [`Detected at ${new Date(pending.time).toLocaleTimeString()}.`, null]
        ]);
        eventTag.textContent = evaluating ? "Automatic Check" : "Awaiting Decision";
        return;
    }

    if (state.history.length === 0) {
        setStatus(decisionTitle, "No Pending Review", null);
        decisionDescription.textContent = "Waiting for a goal-line event.";
        eventTag.textContent = "Waiting";
        return;
    }

    // The most recent decision, including a correction of an older automatic goal.
    const last = state.history.reduce((a, b) => (b.time > a.time ? b : a));
    const time = ` Event #${last.number}, ${new Date(last.time).toLocaleTimeString()}.`;

    if (last.corrected) {
        setStatus(decisionTitle, "No Goal", "bad");
        setDescription([["Automatic goal overturned by the referee." + time, null]]);
    } else if (last.auto) {
        setStatus(decisionTitle, "Automatic Goal", "ok");
        setDescription([["IR interruption and confident soccer ball detection." + time, null],
            ...cameraParts(last)]);
    } else if (last.decision === "GOAL") {
        setStatus(decisionTitle, "Goal", "ok");
        setDescription([["Confirmed by referee." + time, null]]);
    } else {
        setStatus(decisionTitle, "No Goal", "bad");
        setDescription([["Rejected by referee." + time, null]]);
    }
    eventTag.textContent = last.auto ? "Automatic" : "Referee";
}

function renderReplay(board) {
    const replay = state && state.replay;
    const canControl = socket.connected && board.connected && replayEvent;

    replayControls.hidden = !replayEvent;

    if (!replayEvent) {
        return;
    }

    if (!replay || replay.frames === 0) {
        replayLabel.textContent = "No replay footage";
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
    const event = replayEvent;

    if (event.cameraReplay === "pending") {
        return "Waiting for the camera replay...";
    }

    if (event.cameraReplay === "unavailable") {
        return event.camera === "ignored"
            ? "Replay unavailable: the camera ignored this trigger"
            : "Replay unavailable for this event";
    }

    return event.cameraReplay === "ready" ? "Loading replay..." : "No replay footage for this event";
}

// The server relays the board's stream at /camera/stream. A new query string
// on every attach stops the browser reusing a stale cached image.
function showStream(url) {
    if (url && cameraStream.dataset.src !== url) {
        cameraStream.dataset.src = url;
        cameraStream.src = `/camera/stream?n=${++streamAttempt}`;
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
    } else if (replayEvent) {
        tag = viewing ? "Automatic Goal Replay" : "Recorded Replay";
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
            `Camera stream - ${stats.fps} fps, model ${stats.modelMs} ms, ball: ${stats.ball ? "yes" : "no"}` +
            ` - ${new URL(board.stream).host}`;
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
        const how = event.corrected ? "automatic, corrected by referee" : event.auto ? "automatic" : "referee";
        const type = event.manual ? "manual review" : sourcesText(event.sources);
        label.append(`Event #${event.number} - `, decision, ` - ${how} (${type}`);

        if (event.camera && event.camera !== "pending") {
            const camera = document.createElement("span");
            const best = event.confidence && event.confidence.best > 0 ? ` ${event.confidence.best}%` : "";
            camera.textContent = `camera: ${CAMERA_EVIDENCE[event.camera][0]}${best}`;
            camera.className = CAMERA_EVIDENCE[event.camera][1] || "";
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

function decide(result) {
    if (socket.connected && pending && pending.id !== decidedId) {
        decidedId = pending.id;
        socket.emit("decision", { id: pending.id, result });
        render();
    }
}

confirmBtn.addEventListener("click", () => decide("GOAL"));
rejectBtn.addEventListener("click", () => decide("NO GOAL"));

viewBtn.addEventListener("click", () => {
    const auto = latestAutoGoal();
    socket.emit("view", viewing || !auto ? null : { id: auto.id });
});

correctBtn.addEventListener("click", () => {
    const auto = latestAutoGoal();
    if (auto && window.confirm(`Change Event #${auto.number} from an automatic goal to No Goal?`)) {
        socket.emit("correct", { id: auto.id });
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
