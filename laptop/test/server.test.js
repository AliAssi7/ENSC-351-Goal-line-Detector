const test = require("node:test");
const assert = require("node:assert");
const http = require("http");
const net = require("net");
const { startServer, validCameraHost, streamUrl } = require("../server");

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function waitUntil(check, what, timeout = 2000) {
    return new Promise((resolve, reject) => {
        const started = Date.now();
        const poll = () => {
            const result = check();
            if (result) {
                resolve(result);
            } else if (Date.now() - started > timeout) {
                reject(new Error(`Timed out waiting for ${what}`));
            } else {
                setTimeout(poll, 10);
            }
        };
        poll();
    });
}

// Minimal socket.io client over the WebSocket transport.
class Dashboard {
    constructor(port) {
        this.latest = {};
        this.counts = {};
        this.ws = new WebSocket(`ws://127.0.0.1:${port}/socket.io/?EIO=4&transport=websocket`);
        this.ws.onmessage = ({ data }) => {
            if (data[0] === "0") {
                this.ws.send("40");
            } else if (data === "2") {
                this.ws.send("3");
            } else if (data.startsWith("42")) {
                const [name, payload] = JSON.parse(data.slice(2));
                this.latest[name] = payload;
                this.counts[name] = (this.counts[name] || 0) + 1;
            }
        };
    }

    emit(name, payload) {
        this.ws.send("42" + JSON.stringify(payload === undefined ? [name] : [name, payload]));
    }

    state(check, what) {
        return waitUntil(() => this.latest.update && check(this.latest.update) && this.latest.update, what);
    }

    close() {
        this.ws.close();
    }
}

class Board {
    constructor(port) {
        this.lines = [];
        this.buffer = "";
        this.closed = false;
        this.socket = net.connect(port, "127.0.0.1");
        this.socket.setEncoding("utf8");
        this.socket.on("data", (data) => {
            this.buffer += data;
            const lines = this.buffer.split("\n");
            this.buffer = lines.pop();
            this.lines.push(...lines.filter((l) => l !== "PING"));
        });
        this.socket.on("close", () => { this.closed = true; });
        this.ready = new Promise((resolve) => this.socket.on("connect", resolve));
    }

    send(line) {
        this.socket.write(line + "\n");
    }

    frame(kind, id, index, width, height, bytes, format = "rgb") {
        this.socket.write(`FRAME ${kind} ${id} ${index} ${width} ${height} 5 6 4 3 ${bytes.length} ${format}\n`);
        this.socket.write(bytes);
    }

    take(line) {
        return waitUntil(() => {
            const i = this.lines.indexOf(line);
            return i >= 0 && this.lines.splice(i, 1);
        }, `board to receive "${line}"`);
    }

    close() {
        this.socket.destroy();
    }
}

async function setup(options = {}) {
    const server = await startServer({ httpPort: 0, boardPort: 0, cameraHost: "", ...options });
    const dashboard = new Dashboard(server.httpPort);
    await dashboard.state(() => true, "initial update");
    return { server, dashboard };
}

test("no simulated reviews without the board", async () => {
    const { server, dashboard } = await setup();

    try {
        const initial = dashboard.latest.update;
        assert.strictEqual(initial.board.connected, false);
        assert.strictEqual(initial.pending, null);

        // Simulate Event needs the real board: nothing happens without it.
        dashboard.emit("simulateGoal");
        dashboard.emit("decision", { id: "x", result: "GOAL" });
        dashboard.emit("decision", "nonsense");
        dashboard.emit("replay", { action: "next" });
        await sleep(150);
        const state = dashboard.latest.update;
        assert.strictEqual(state.pending, null);
        assert.strictEqual(state.goals, 0);
        assert.strictEqual(state.history.length, 0);
    } finally {
        dashboard.close();
        server.close();
    }
});

test("board events, replay and decisions", async () => {
    const { server, dashboard } = await setup();
    const board = new Board(server.boardPort);

    try {
        await board.ready;
        await dashboard.state((s) => s.board.connected, "board connected");

        board.send("STATUS monitoring live ok -");
        board.send("LINE 192 0 192 239 288 120");
        await dashboard.state((s) => s.board.camera === "live" && s.line, "status and line");
        assert.deepStrictEqual(dashboard.latest.update.line, { x1: 192, y1: 0, x2: 192, y2: 239, gx: 288, gy: 120 });

        const live = Buffer.alloc(4 * 2 * 3, 7);
        board.frame("LIVE", "-", 0, 4, 2, live);
        await waitUntil(() => dashboard.latest.live, "live frame");
        assert.deepStrictEqual(Buffer.from(dashboard.latest.live.data, "base64"), live);
        assert.deepStrictEqual(dashboard.latest.live.ball, { x: 5, y: 6, r: 4, zone: 3 });

        board.send("EVENT 1700-1 ir");
        let state = await dashboard.state((s) => s.pending, "camera event");
        assert.strictEqual(state.pending.id, "1700-1");

        board.send("EVENT 1700-1 ir,camera");
        state = await dashboard.state((s) => s.pending.sources.length === 2, "camera evidence");
        assert.deepStrictEqual(state.pending.sources, ["ir", "camera"]);
        assert.strictEqual(state.pending.manual, false);
        assert.strictEqual(state.pending.phase, "evaluating");

        // Replay updates for another event or out of range are ignored.
        board.send("REPLAY other 1 10 0 0");
        board.send("REPLAY 1700-1 10 10 0 0");
        board.send("REPLAY 1700-1 4 10 1 0");
        state = await dashboard.state((s) => s.replay, "replay state");
        assert.deepStrictEqual(state.replay, { frame: 4, frames: 10, playing: true, complete: false });

        const replayBytes = Buffer.from([...Array(6 * 3).keys()]);
        board.frame("REPLAY", "1700-1", 4, 3, 2, replayBytes);
        board.frame("REPLAY", "other", 1, 3, 2, Buffer.alloc(18));
        await waitUntil(() => dashboard.latest.replayFrame, "replay frame");
        await sleep(50);
        assert.strictEqual(dashboard.latest.replayFrame.id, "1700-1");
        assert.deepStrictEqual(Buffer.from(dashboard.latest.replayFrame.data, "base64"), replayBytes);

        dashboard.emit("replay", { action: "next" });
        await board.take("REPLAY NEXT");
        dashboard.emit("replay", { action: "pause" });
        await board.take("REPLAY PAUSE");
        dashboard.emit("replay", { action: "seek", frame: 9 });
        await board.take("REPLAY SEEK 9");
        dashboard.emit("replay", { action: "seek", frame: 10 });
        dashboard.emit("replay", { action: "seek", frame: "3" });
        dashboard.emit("replay", { action: "rewind" });
        await sleep(100);
        assert.ok(!board.lines.some((l) => l.startsWith("REPLAY")));

        // A new dashboard recovers the event, replay and frame.
        const second = new Dashboard(server.httpPort);
        await second.state((s) => s.pending && s.replay, "state on reconnect");
        await waitUntil(() => second.latest.replayFrame, "cached replay frame");
        assert.strictEqual(second.latest.update.pending.id, "1700-1");
        second.close();

        // No decision while the board is still evaluating the evidence.
        dashboard.emit("decision", { id: "1700-1", result: "GOAL" });
        await sleep(100);
        assert.ok(dashboard.latest.update.pending);
        assert.ok(!board.lines.some((l) => l.startsWith("DECISION")));

        board.send("AWAITING 1700-1 bogus");
        board.send("AWAITING 1700-1 low_confidence");
        state = await dashboard.state((s) => s.pending.phase === "awaiting", "awaiting");
        assert.strictEqual(state.pending.reason, "low_confidence");

        // A double click sends the decision twice: it is counted once.
        dashboard.emit("decision", { id: "1700-1", result: "MAYBE" });
        dashboard.emit("decision", { id: "1700-1", result: "GOAL" });
        dashboard.emit("decision", { id: "1700-1", result: "GOAL" });
        await board.take("DECISION 1700-1 GOAL");
        state = await dashboard.state((s) => !s.pending, "decision recorded");
        assert.strictEqual(state.goals, 1);
        assert.strictEqual(state.history.length, 1);
        assert.deepStrictEqual(state.history[0].sources, ["ir", "camera"]);
        assert.strictEqual(state.history[0].auto, false);
        assert.strictEqual(state.history[0].reason, "low_confidence");
        assert.strictEqual(state.replay, null);
        await sleep(50);
        assert.ok(!board.lines.some((l) => l.startsWith("DECISION")));

        dashboard.emit("decision", { id: "1700-1", result: "NO GOAL" });
        await sleep(100);
        assert.ok(!board.lines.some((l) => l.startsWith("DECISION")));

        // A late EVENT for the decided id gets the decision again, not a new review.
        board.send("EVENT 1700-1 ir,camera");
        await board.take("DECISION 1700-1 GOAL");
        assert.strictEqual(dashboard.latest.update.pending, null);

        // Manual review goes through the board.
        dashboard.emit("simulateGoal");
        await board.take("REVIEW");
        board.send("EVENT 1700-2 manual");
        board.send("AWAITING 1700-2 manual");
        state = await dashboard.state((s) => s.pending && s.pending.phase === "awaiting", "manual event");
        assert.strictEqual(state.pending.manual, true);
        assert.strictEqual(state.pending.number, 2);

        dashboard.emit("decision", { id: "1700-2", result: "NO GOAL" });
        await board.take("DECISION 1700-2 NO_GOAL");
        state = await dashboard.state((s) => s.history.length === 2, "second decision");
        assert.strictEqual(state.goals, 1);
        assert.strictEqual(state.history[0].decision, "NO GOAL");
        assert.strictEqual(state.history[0].number, 2);

        // Simulate Event while a review is pending does nothing.
        board.send("EVENT 1700-3 ir");
        board.send("AWAITING 1700-3 no_ball");
        await dashboard.state((s) => s.pending && s.pending.phase === "awaiting", "third event");
        dashboard.emit("simulateGoal");
        await sleep(100);
        assert.ok(!board.lines.includes("REVIEW"));
        dashboard.emit("decision", { id: "1700-3", result: "NO GOAL" });
        await board.take("DECISION 1700-3 NO_GOAL");

        // Calibration is validated before it reaches the board.
        dashboard.emit("calibrate", { x1: 10, y1: 0, x2: 10, y2: 100, gx: 50, gy: 50 });
        await board.take("LINE 10 0 10 100 50 50");
        dashboard.emit("calibrate", { x1: -1, y1: 0, x2: 10, y2: 100, gx: 50, gy: 50 });
        dashboard.emit("calibrate", { x1: 1 });
        await sleep(100);
        assert.ok(!board.lines.some((l) => l.startsWith("LINE")));
    } finally {
        board.close();
        dashboard.close();
        server.close();
    }
});

test("YOLO camera evidence, stream and JPEG replay frames", async () => {
    const { server, dashboard } = await setup();
    const board = new Board(server.boardPort);

    try {
        await board.ready;
        board.send("STATUS monitoring yolo ok -");
        board.send("STREAM 8080");
        board.send("CAMSTAT 12 310 1");
        let state = await dashboard.state((s) => s.board.stream && s.board.cameraStats, "stream and stats");
        assert.strictEqual(state.board.camera, "yolo");
        assert.strictEqual(state.board.stream, "http://127.0.0.1:8080/stream");
        assert.deepStrictEqual(state.board.cameraStats, { fps: 12, modelMs: 310, ball: true });

        board.send("STREAM 99999");
        board.send("CAMSTAT x 1 1");
        await sleep(100);
        assert.strictEqual(dashboard.latest.update.board.stream, "http://127.0.0.1:8080/stream");

        board.send("EVENT 2000-1 ir");
        board.send("AWAITING 2000-1 unverified");
        board.send("CAMERA 2000-1 pending pending");
        state = await dashboard.state((s) => s.pending && s.pending.camera === "pending", "camera pending");
        assert.strictEqual(state.pending.cameraReplay, "pending");

        board.send("CAMERA 2000-1 maybe pending");
        board.send("CAMERA 2000-1 seen lost");
        board.send("CAMERA 2000-1 seen");
        board.send("CAMERA other seen ready");
        board.send("CAMERA 2000-1 seen ready");
        state = await dashboard.state((s) => s.pending.camera === "seen", "camera seen");
        assert.strictEqual(state.pending.cameraReplay, "ready");

        const jpeg = Buffer.from([0xff, 0xd8, 1, 2, 3, 0xff, 0xd9]);
        board.send("REPLAY 2000-1 0 80 0 1");
        board.frame("REPLAY", "2000-1", 0, 640, 480, jpeg, "jpeg");
        await waitUntil(() => dashboard.latest.replayFrame, "jpeg replay frame");
        assert.strictEqual(dashboard.latest.replayFrame.format, "jpeg");
        assert.deepStrictEqual(Buffer.from(dashboard.latest.replayFrame.data, "base64"), jpeg);

        dashboard.emit("decision", { id: "2000-1", result: "GOAL" });
        await board.take("DECISION 2000-1 GOAL");
        state = await dashboard.state((s) => s.history.length === 1, "history");
        assert.strictEqual(state.history[0].camera, "seen");
        assert.strictEqual(state.history[0].cameraReplay, "ready");
        assert.deepStrictEqual(state.history[0].sources, ["ir"]);

        board.send("STREAM -");
        await dashboard.state((s) => s.board.stream === null, "stream removed");

        // An unknown frame format closes the connection.
        board.send("FRAME LIVE - 0 4 2 -1 -1 0 0 24 png");
        await waitUntil(() => board.closed, "closed after bad format");
        state = await dashboard.state((s) => !s.board.connected, "disconnected");
        assert.strictEqual(state.board.stream, null);
        assert.strictEqual(state.board.cameraStats, null);
    } finally {
        board.close();
        dashboard.close();
        server.close();
    }
});

test("board reconnect during a review", async () => {
    const { server, dashboard } = await setup();
    let board = new Board(server.boardPort);

    try {
        await board.ready;
        board.send("EVENT 1800-5 ir");
        board.send("AWAITING 1800-5 timeout");
        await dashboard.state((s) => s.pending && s.pending.phase === "awaiting", "IR event");

        board.close();
        let state = await dashboard.state((s) => !s.board.connected, "board disconnected");
        assert.strictEqual(state.pending.id, "1800-5");

        // Referee decides while the board is away.
        dashboard.emit("decision", { id: "1800-5", result: "GOAL" });
        state = await dashboard.state((s) => !s.pending, "decision while offline");
        assert.strictEqual(state.goals, 1);

        board = new Board(server.boardPort);
        await board.ready;
        board.send("EVENT 1800-5 ir");
        await board.take("DECISION 1800-5 GOAL");
        await sleep(50);
        assert.strictEqual(dashboard.latest.update.pending, null);
        assert.strictEqual(dashboard.latest.update.goals, 1);

        // A board restarted mid-review reports a new event: the stale one is replaced.
        board.send("EVENT 1800-6 ir");
        board.send("AWAITING 1800-6 no_ball");
        await dashboard.state((s) => s.pending && s.pending.id === "1800-6", "event 6");
        board.send("EVENT 1900-1 ir");
        board.send("AWAITING 1900-1 no_ball");
        state = await dashboard.state((s) => s.pending && s.pending.id === "1900-1" &&
            s.pending.phase === "awaiting", "stale event replaced");
        assert.deepStrictEqual(state.pending.sources, ["ir"]);
        dashboard.emit("decision", { id: "1800-6", result: "GOAL" });
        dashboard.emit("decision", { id: "1900-1", result: "NO GOAL" });
        await board.take("DECISION 1900-1 NO_GOAL");
        await sleep(50);
        assert.ok(!board.lines.includes("DECISION 1800-6 GOAL"));

        // A newer connection replaces the old one.
        const replacement = new Board(server.boardPort);
        await replacement.ready;
        await waitUntil(() => board.closed, "old board connection closed");
        await dashboard.state((s) => s.board.connected, "still connected");
        replacement.close();
    } finally {
        board.close();
        dashboard.close();
        server.close();
    }
});

test("invalid board data is rejected", async () => {
    const { server, dashboard } = await setup();

    try {
        let board = new Board(server.boardPort);
        await board.ready;

        board.send("EVENT bad/id camera");
        board.send("EVENT 1-1 telepathy");
        board.send("EVENT 1-1");
        board.send("STATUS dancing live ok -");
        board.send("STREAM 8080 not-an-ip");
        board.send("LINE 1 2 3");
        board.send("REPLAY 1-1 x 10 0 0");
        board.send("HELLO");
        await sleep(100);
        let state = dashboard.latest.update;
        assert.strictEqual(state.pending, null);
        assert.strictEqual(state.board.state, null);
        assert.strictEqual(state.line, null);
        assert.strictEqual(board.closed, false);

        // A frame header whose size does not match closes the connection.
        board.send("FRAME LIVE - 0 4 2 -1 -1 0 0 999 rgb");
        await waitUntil(() => board.closed, "connection closed after bad frame");

        // So does a line that never ends.
        board = new Board(server.boardPort);
        await board.ready;
        board.socket.write("x".repeat(2000));
        await waitUntil(() => board.closed, "connection closed after long line");

        state = await dashboard.state((s) => !s.board.connected, "board marked disconnected");
        assert.strictEqual(state.pending, null);
    } finally {
        dashboard.close();
        server.close();
    }
});

test("camera host validation and stream URL", () => {
    for (const ok of ["192.168.137.76", "beagle.local", "BeagleY-AI", "fe80::1"]) {
        assert.ok(validCameraHost(ok), ok);
    }
    for (const bad of ["", "http://192.168.137.76", "192.168.137.76:8080", "a b", "x/y", "-bad", undefined]) {
        assert.ok(!validCameraHost(bad), String(bad));
    }
    assert.strictEqual(streamUrl("192.168.137.76", 8080), "http://192.168.137.76:8080/stream");
    assert.strictEqual(streamUrl("fe80::1", 8080), "http://[fe80::1]:8080/stream");
});

test("camera address: CAMERA_HOST, then the board's own address, then the connection", async () => {
    await assert.rejects(startServer({ httpPort: 0, boardPort: 0, cameraHost: "http://1.2.3.4:8080" }),
        /Invalid CAMERA_HOST/);

    // The regression: behind NAT the board connects from the server's own
    // address (172.16.176.79 in testing); the board's reported IP must win.
    let { server, dashboard } = await setup();
    let board = new Board(server.boardPort);

    try {
        await board.ready;
        board.send("STREAM 8080 192.168.137.76");
        let state = await dashboard.state((s) => s.board.stream, "reported stream");
        assert.strictEqual(state.board.stream, "http://192.168.137.76:8080/stream");

        // An older board that does not report its address falls back to the connection.
        board.send("STREAM 8080");
        state = await dashboard.state((s) => s.board.stream.includes("127.0.0.1"), "fallback");
        assert.strictEqual(state.board.stream, "http://127.0.0.1:8080/stream");

        // After the board reconnects the stream is restored.
        board.close();
        await dashboard.state((s) => !s.board.connected && s.board.stream === null, "stream cleared");
        board = new Board(server.boardPort);
        await board.ready;
        board.send("STREAM 8080 192.168.137.76");
        await dashboard.state((s) => s.board.stream === "http://192.168.137.76:8080/stream", "restored");
    } finally {
        board.close();
        dashboard.close();
        server.close();
    }

    ({ server, dashboard } = await setup({ cameraHost: " 10.0.0.5 " }));
    board = new Board(server.boardPort);

    try {
        await board.ready;
        board.send("STREAM 8081 192.168.137.76");
        const state = await dashboard.state((s) => s.board.stream, "override");
        assert.strictEqual(state.board.stream, "http://10.0.0.5:8081/stream");
    } finally {
        board.close();
        dashboard.close();
        server.close();
    }
});

function get(port, path) {
    return new Promise((resolve, reject) => {
        const req = http.get({ host: "127.0.0.1", port, path }, (res) => {
            const chunks = [];
            res.on("data", (c) => chunks.push(c));
            res.on("end", () => resolve({ res, body: Buffer.concat(chunks), aborted: false }));
            res.on("aborted", () => resolve({ res, body: Buffer.concat(chunks), aborted: true }));
            res.on("error", () => resolve({ res, body: Buffer.concat(chunks), aborted: true }));
        });
        req.on("error", reject);
    });
}

test("the server relays the camera stream", async () => {
    // Stands in for goal_cam.py's MJPEG server: two parts, then it stops.
    const camera = http.createServer((req, res) => {
        if (req.url !== "/stream") {
            res.writeHead(404).end();
            return;
        }
        res.writeHead(200, { "Content-Type": "multipart/x-mixed-replace; boundary=frame" });
        res.write("--frame\r\nContent-Type: image/jpeg\r\n\r\nJPEG1\r\n");
        setTimeout(() => res.end("--frame\r\nContent-Type: image/jpeg\r\n\r\nJPEG2\r\n"), 50);
    });
    await new Promise((resolve) => camera.listen(0, "127.0.0.1", resolve));
    const cameraPort = camera.address().port;

    const { server, dashboard } = await setup();
    const board = new Board(server.boardPort);

    try {
        // No stream known yet.
        let reply = await get(server.httpPort, "/camera/stream?n=1");
        assert.strictEqual(reply.res.statusCode, 503);

        await board.ready;
        board.send(`STREAM ${cameraPort} 127.0.0.1`);
        await dashboard.state((s) => s.board.stream, "stream");

        // The query string (a browser cache-buster) is not passed to the camera.
        reply = await get(server.httpPort, "/camera/stream?n=2");
        assert.strictEqual(reply.res.statusCode, 200);
        assert.match(reply.res.headers["content-type"], /multipart\/x-mixed-replace/);
        assert.strictEqual(reply.res.headers["cache-control"], "no-store");
        assert.match(reply.body.toString(), /JPEG1[\s\S]*JPEG2/);
        assert.ok(reply.aborted, "a finished camera stream ends abruptly so the browser retries");

        // Camera unreachable: 502, and the server keeps running.
        camera.close();
        await new Promise((resolve) => setTimeout(resolve, 50));
        reply = await get(server.httpPort, "/camera/stream?n=3");
        assert.strictEqual(reply.res.statusCode, 502);
    } finally {
        camera.close();
        board.close();
        dashboard.close();
        server.close();
    }
});

test("IR degraded status is shown", async () => {
    const { server, dashboard } = await setup();
    const board = new Board(server.boardPort);

    try {
        await board.ready;
        board.send("STATUS monitoring yolo degraded -");
        const state = await dashboard.state((s) => s.board.ir === "degraded", "degraded IR");
        assert.strictEqual(state.board.camera, "yolo");
    } finally {
        board.close();
        dashboard.close();
        server.close();
    }
});

test("automatic goals: recorded once, replayable and correctable once", async () => {
    const { server, dashboard } = await setup();
    let board = new Board(server.boardPort);

    try {
        await board.ready;
        board.send("STATUS evaluating yolo degraded 3000-1");
        board.send("EVENT 3000-1 ir");
        board.send("CAMERA 3000-1 pending pending");
        let state = await dashboard.state((s) => s.pending && s.pending.phase === "evaluating", "evaluating");
        assert.strictEqual(state.board.state, "evaluating");

        // AUTO is only accepted for IR events.
        board.send("AUTO 3000-1 manual");
        board.send("AUTO 3000-1 camera");
        await sleep(100);
        assert.strictEqual(dashboard.latest.update.goals, 0);

        board.send("CAMERA 3000-1 confident pending");
        board.send("CONFIDENCE 3000-1 6 3 91 -900 100");
        board.send("CONFIDENCE 3000-1 6 3 91 -900 100 120");
        board.send("AUTO 3000-1 ir");
        await board.take("DECISION 3000-1 GOAL");
        state = await dashboard.state((s) => !s.pending && s.goals === 1, "automatic goal");
        const entry = state.history[0];
        assert.strictEqual(entry.auto, true);
        assert.strictEqual(entry.decision, "GOAL");
        assert.strictEqual(entry.camera, "confident");
        assert.deepStrictEqual(entry.confidence, { runs: 6, ballRuns: 3, best: 91, firstMs: -900, lastMs: 100, movePx: 120 });

        // A repeated AUTO (e.g. after a reconnect) is acknowledged, not counted again.
        board.send("AUTO 3000-1 ir");
        await board.take("DECISION 3000-1 GOAL");
        await sleep(50);
        assert.strictEqual(dashboard.latest.update.goals, 1);
        assert.strictEqual(dashboard.latest.update.history.length, 1);

        // Its replay arrives after the decision.
        board.send("CAMERA 3000-1 confident ready");
        state = await dashboard.state((s) => s.history[0].cameraReplay === "ready", "late replay");

        // The referee watches it with the replay controls.
        dashboard.emit("view", { id: "nope" });
        dashboard.emit("view", { id: "3000-1" });
        await board.take("VIEW 3000-1");
        board.send("REPLAY 3000-1 0 8 0 1");
        board.frame("REPLAY", "3000-1", 0, 640, 480, Buffer.from([0xff, 0xd8, 1, 0xff, 0xd9]), "jpeg");
        state = await dashboard.state((s) => s.viewing === "3000-1" && s.replay, "viewing");
        await waitUntil(() => dashboard.latest.replayFrame && dashboard.latest.replayFrame.id === "3000-1", "frame");
        dashboard.emit("replay", { action: "next" });
        await board.take("REPLAY NEXT");
        dashboard.emit("view", null);
        await board.take("VIEW -");
        state = await dashboard.state((s) => s.viewing === null, "back to live");

        // Correct it to No Goal: the same entry changes, one goal less, once.
        dashboard.emit("correct", { id: "3000-1" });
        dashboard.emit("correct", { id: "3000-1" });
        await board.take("DECISION 3000-1 NO_GOAL");
        state = await dashboard.state((s) => s.goals === 0, "corrected");
        assert.strictEqual(state.history.length, 1);
        assert.strictEqual(state.history[0].decision, "NO GOAL");
        assert.strictEqual(state.history[0].corrected, true);
        await sleep(100);
        assert.ok(!board.lines.some((l) => l.startsWith("DECISION")));
        assert.strictEqual(dashboard.latest.update.goals, 0);

        // A late AUTO after the correction gets the corrected decision back.
        board.send("AUTO 3000-1 ir");
        await board.take("DECISION 3000-1 NO_GOAL");
        assert.strictEqual(dashboard.latest.update.goals, 0);

        // Manual events cannot be corrected or viewed this way.
        board.send("EVENT 3000-2 manual");
        board.send("AWAITING 3000-2 manual");
        await dashboard.state((s) => s.pending && s.pending.phase === "awaiting", "manual awaiting");
        dashboard.emit("decision", { id: "3000-2", result: "GOAL" });
        await board.take("DECISION 3000-2 GOAL");
        dashboard.emit("correct", { id: "3000-2" });
        dashboard.emit("view", { id: "3000-2" });
        await sleep(100);
        assert.strictEqual(dashboard.latest.update.goals, 1);
        assert.strictEqual(dashboard.latest.update.history[0].manual, true);
        assert.ok(!board.lines.some((l) => l.startsWith("VIEW")));

        // An automatic goal sent while the server was restarting is still recorded.
        board.close();
        await dashboard.state((s) => !s.board.connected, "disconnected");
        board = new Board(server.boardPort);
        await board.ready;
        board.send("AUTO 3000-3 ir,camera");
        await board.take("DECISION 3000-3 GOAL");
        state = await dashboard.state((s) => s.goals === 2, "auto after reconnect");
        assert.strictEqual(state.history[0].id, "3000-3");
        assert.strictEqual(state.history[0].number, 3);

        // No correction while another event is pending.
        board.send("EVENT 3000-4 ir");
        await dashboard.state((s) => s.pending && s.pending.id === "3000-4", "pending");
        dashboard.emit("correct", { id: "3000-3" });
        await sleep(100);
        assert.strictEqual(dashboard.latest.update.goals, 2);
    } finally {
        board.close();
        dashboard.close();
        server.close();
    }
});

test("a pending manual decision survives a board reconnect", async () => {
    const { server, dashboard } = await setup();
    let board = new Board(server.boardPort);

    try {
        await board.ready;
        board.send("EVENT 4000-1 manual");
        board.send("AWAITING 4000-1 manual");
        await dashboard.state((s) => s.pending && s.pending.phase === "awaiting", "awaiting");

        board.close();
        await dashboard.state((s) => !s.board.connected, "disconnected");
        board = new Board(server.boardPort);
        await board.ready;
        board.send("EVENT 4000-1 manual");
        board.send("AWAITING 4000-1 manual");
        await dashboard.state((s) => s.board.connected, "reconnected");
        await sleep(50);
        const state = dashboard.latest.update;
        assert.strictEqual(state.pending.id, "4000-1");
        assert.strictEqual(state.pending.number, 1);
        assert.strictEqual(state.pending.phase, "awaiting");

        dashboard.emit("decision", { id: "4000-1", result: "NO GOAL" });
        await board.take("DECISION 4000-1 NO_GOAL");
        await dashboard.state((s) => !s.pending && s.goals === 0 && s.history.length === 1, "decided");
    } finally {
        board.close();
        dashboard.close();
        server.close();
    }
});
