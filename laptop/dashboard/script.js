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
const systemStatus = document.querySelector(".system-status");

let pending = null;

simulateBtn.disabled = true;
confirmBtn.disabled = true;
rejectBtn.disabled = true;

socket.on("connect", () => {
    systemStatus.textContent = "Demo Mode - Connected";
});

socket.on("disconnect", () => {
    systemStatus.textContent = "Server Disconnected";
    simulateBtn.disabled = true;
    confirmBtn.disabled = true;
    rejectBtn.disabled = true;
});

socket.on("update", (state) => {
    pending = state.pending;
    goalCount.textContent = state.goals;

    simulateBtn.disabled = !socket.connected || pending !== null;
    confirmBtn.disabled = !socket.connected || pending === null;
    rejectBtn.disabled = !socket.connected || pending === null;

    if (pending) {
        reviewStatus.textContent = "Pending";
        decisionTitle.textContent = "Potential Goal";
        decisionDescription.textContent = "Goal-line event detected.";
        eventTag.textContent = "Review Required";
    } else {
        reviewStatus.textContent = "Ready";

        if (state.history.length > 0) {
            const last = state.history[0];

            decisionTitle.textContent =
                last.decision === "GOAL" ? "Goal Confirmed" : "No Goal";

            decisionDescription.textContent = "Review completed.";
            eventTag.textContent = "Reviewed";
        } else {
            decisionTitle.textContent = "No Pending Review";
            decisionDescription.textContent = "Waiting for a goal-line event.";
            eventTag.textContent = "Waiting";
        }
    }

    showHistory(state.history);
});

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
        label.textContent = `Event #${event.id} - ${event.decision}`;

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