
const simulateBtn = document.getElementById("simulateBtn");
const confirmBtn = document.getElementById("confirmBtn");
const rejectBtn = document.getElementById("rejectBtn");

const reviewStatus = document.getElementById("reviewStatus");
const decisionTitle = document.getElementById("decisionTitle");
const decisionDescription = document.getElementById("decisionDescription");
const eventTag = document.getElementById("eventTag");
const eventHistory = document.getElementById("eventHistory");
const goalCount = document.getElementById("goalCount");

let goals = 0;
let pending = false;
let eventNumber = 0;

simulateBtn.addEventListener("click", () => {
    if (pending) return;

    pending = true;
    eventNumber++;

    reviewStatus.textContent = "Pending";
    decisionTitle.textContent = "Potential Goal";
    decisionDescription.textContent = "Simulated goal-line event detected.";
    eventTag.textContent = "Review Required";

    confirmBtn.disabled = false;
    rejectBtn.disabled = false;
});

function finishReview(decision) {
    if (!pending) return;

    pending = false;

    const confirmed = decision === "GOAL";
    if (confirmed) goals++;

    goalCount.textContent = goals;
    reviewStatus.textContent = "Ready";
    decisionTitle.textContent = confirmed ? "Goal Confirmed" : "No Goal";
    decisionDescription.textContent = "Review completed.";
    eventTag.textContent = "Reviewed";

    confirmBtn.disabled = true;
    rejectBtn.disabled = true;

    if (eventNumber === 1) eventHistory.textContent = "";

    const entry = document.createElement("div");
    entry.className = "history-entry";

    const label = document.createElement("span");
    label.textContent = `Event #${eventNumber} — ${decision}`;

    const time = document.createElement("span");
    time.textContent = new Date().toLocaleTimeString();

    entry.append(label, time);
    eventHistory.prepend(entry);
}

confirmBtn.addEventListener("click", () => finishReview("GOAL"));
rejectBtn.addEventListener("click", () => finishReview("NO GOAL"));
