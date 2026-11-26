const express = require("express");
const http = require("http");
const net = require("net");
const { Server } = require("socket.io");
const path = require("path");

const ID_PATTERN = /^[A-Za-z0-9-]{1,40}$/;
const SOURCES = ["camera", "ir", "manual"];
const BOARD_STATES = ["monitoring", "review", "goal"];
const CAMERA_STATES = ["yolo", "starting", "live", "file", "sim", "offline", "disabled"];
const CAMERA_EVIDENCE = ["pending", "seen", "missed", "ignored", "unavailable"];
const CAMERA_REPLAY = ["pending", "ready", "unavailable"];
const IR_STATES = ["ok", "degraded", "error", "disabled", "offline"];
const REPLAY_ACTIONS = { next: "NEXT", prev: "PREV", play: "PLAY", pause: "PAUSE" };
const MAX_DIMENSION = 1920;
const MAX_JPEG_BYTES = 4 * 1024 * 1024;
const BOARD_TIMEOUT_MS = 6000;
const MAX_DECIDED = 200;
const CAMERA_TIMEOUT_MS = 5000;
const HOSTNAME = /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$/;

// CAMERA_HOST must be a bare IP address or host name, e.g. 192.168.137.76.
function validCameraHost(host) {
    return typeof host === "string" && (net.isIP(host) !== 0 || HOSTNAME.test(host));
}

function streamUrl(host, port) {
    return `http://${net.isIPv6(host) ? `[${host}]` : host}:${port}/stream`;
}

function isInt(value, min, max) {
    return Number.isInteger(value) && value >= min && value <= max;
}

function parseInts(parts) {
    const values = parts.map(Number);
    return values.every((v, i) => /^-?\d+$/.test(parts[i]) && Number.isSafeInteger(v))
        ? values
        : null;
}

// FRAME <LIVE|REPLAY> <id> <index> <w> <h> <ballX> <ballY> <ballR> <zone> <bytes> <rgb|jpeg>
function parseFrameHeader(line) {
    const parts = line.split(" ");

    if (parts[0] !== "FRAME") {
        return null;
    }

    const numbers = parts.length === 12 ? parseInts(parts.slice(3, 11)) : null;
    const format = parts[11];

    if (!numbers || !["LIVE", "REPLAY"].includes(parts[1]) ||
        !(parts[2] === "-" || ID_PATTERN.test(parts[2])) || !["rgb", "jpeg"].includes(format)) {
        return false;
    }

    const [index, width, height, ballX, ballY, ballR, zone, length] = numbers;
    const sizeOk = format === "rgb"
        ? length === width * height * 3
        : isInt(length, 4, MAX_JPEG_BYTES);

    if (!isInt(width, 1, MAX_DIMENSION) || !isInt(height, 1, MAX_DIMENSION) ||
        !sizeOk || !isInt(zone, 0, 3) || index < 0) {
        return false;
    }

    return {
        kind: parts[1],
        id: parts[2],
        index,
        width,
        height,
        ball: ballR > 0 ? { x: ballX, y: ballY, r: ballR, zone } : null,
        length,
        format
    };
}

function startServer({ httpPort = 3000, boardPort = 4000, host = "127.0.0.1",
                       cameraHost = process.env.CAMERA_HOST } = {}) {
    cameraHost = (cameraHost || "").trim();

    if (cameraHost && !validCameraHost(cameraHost)) {
        return Promise.reject(new Error(
            `Invalid CAMERA_HOST "${cameraHost}": use only the BeagleY-AI IP address, e.g. 192.168.137.76`));
    }

    const app = express();
    const server = http.createServer(app);
    const io = new Server(server);

    const state = {
        board: { connected: false, state: null, camera: null, ir: null, stream: null, cameraStats: null },
        pending: null,
        goals: 0,
        history: [],
        replay: null,
        line: null
    };

    // Decisions by event id, so a board that reconnects mid-review is told again.
    const decided = new Map();
    let nextNumber = 1;
    let board = null;
    let loggedStream = null;
    let liveFrame = null;
    let replayFrame = null;

    app.use(express.static(path.join(__dirname, "dashboard")));

    // The browser always loads the camera from here, so it never needs the
    // board's address and a refresh or reconnect always gets the current one.
    app.get("/camera/stream", (req, res) => {
        const url = state.board.stream;

        if (!url) {
            res.status(503).end();
            return;
        }

        const upstream = http.get(url, (camera) => {
            res.writeHead(camera.statusCode, {
                "Content-Type": camera.headers["content-type"] || "application/octet-stream",
                "Cache-Control": "no-store"
            });
            // When the camera stops, end abruptly so the browser reports an error and retries.
            camera.pipe(res, { end: false });
            camera.on("end", () => res.destroy());
        });

        upstream.setTimeout(CAMERA_TIMEOUT_MS, () => upstream.destroy());
        upstream.on("error", () => {
            if (res.headersSent) {
                res.destroy();
            } else {
                res.status(502).end();
            }
        });
        res.on("close", () => upstream.destroy());
    });

    function broadcast() {
        io.emit("update", state);
    }

    function sendToBoard(message) {
        if (board) {
            board.write(message + "\n");
        }
    }

    function openEvent(id, sources) {
        state.pending = { id, number: nextNumber++, sources, camera: null, cameraReplay: null, time: Date.now() };
        state.replay = null;
        replayFrame = null;
        broadcast();
    }

    // Reviews always run on the board, so the LEDs, buzzer and replay are real.
    function requestReview() {
        if (!state.pending && board) {
            sendToBoard("REVIEW");
        }
    }

    function decide(id, result) {
        const pending = state.pending;

        if (!pending || id !== pending.id || !["GOAL", "NO GOAL"].includes(result)) {
            return;
        }

        state.history.unshift({
            id: pending.id,
            number: pending.number,
            decision: result,
            sources: pending.sources,
            camera: pending.camera,
            cameraReplay: pending.cameraReplay,
            detected: pending.time,
            time: Date.now()
        });

        if (result === "GOAL") {
            state.goals++;
        }

        // Kept by id, so a board that reconnects mid-review is told again.
        decided.set(id, result === "GOAL" ? "GOAL" : "NO_GOAL");
        if (decided.size > MAX_DECIDED) {
            decided.delete(decided.keys().next().value);
        }
        sendToBoard(`DECISION ${id} ${decided.get(id)}`);

        state.pending = null;
        state.replay = null;
        replayFrame = null;
        broadcast();
    }

    function handleBoardLine(line, socket) {
        const parts = line.split(" ");
        const type = parts[0];

        if (type === "EVENT" && parts.length === 3 && ID_PATTERN.test(parts[1])) {
            const id = parts[1];
            const sources = [...new Set(parts[2].split(","))];

            if (!sources.every((s) => SOURCES.includes(s))) {
                return;
            }

            if (decided.has(id)) {
                sendToBoard(`DECISION ${id} ${decided.get(id)}`);
            } else if (state.pending && state.pending.id === id) {
                state.pending.sources = sources;
                broadcast();
            } else {
                // The board reviews one event at a time, so a different id
                // means any other pending event no longer exists on it.
                if (state.pending) {
                    console.log(`Event ${state.pending.id} replaced by ${id}`);
                }
                openEvent(id, sources);
            }
        } else if (type === "STATUS" && parts.length === 5) {
            const [, boardState, cameraState, irState, id] = parts;

            if (BOARD_STATES.includes(boardState) && CAMERA_STATES.includes(cameraState) &&
                IR_STATES.includes(irState) && (id === "-" || ID_PATTERN.test(id))) {
                Object.assign(state.board, { state: boardState, camera: cameraState, ir: irState });

                if (cameraState === "offline") {
                    liveFrame = null;
                }

                broadcast();
            }
        } else if (type === "REPLAY" && parts.length === 6) {
            const numbers = parseInts(parts.slice(2));

            if (!numbers || !state.pending || state.pending.id !== parts[1]) {
                return;
            }

            const [frame, frames, playing, complete] = numbers;

            if (isInt(frames, 0, 100000) && isInt(frame, 0, Math.max(0, frames - 1))) {
                state.replay = { frame, frames, playing: playing === 1, complete: complete === 1 };
                broadcast();
            }
        } else if (type === "CAMERA" && parts.length === 4) {
            if (state.pending && state.pending.id === parts[1] &&
                CAMERA_EVIDENCE.includes(parts[2]) && CAMERA_REPLAY.includes(parts[3])) {
                state.pending.camera = parts[2];
                state.pending.cameraReplay = parts[3];
                broadcast();
            }
        } else if (type === "STREAM" && (parts.length === 2 || parts.length === 3)) {
            // STREAM <port> [board IP] or STREAM -
            const port = Number(parts[1]);

            if (parts[1] === "-") {
                state.board.stream = null;
            } else if (/^\d+$/.test(parts[1]) && isInt(port, 1, 65535)) {
                // CAMERA_HOST wins; then the address the board reports for itself;
                // the address it connected from is wrong behind NAT.
                const reported = parts[2] && net.isIP(parts[2]) ? parts[2] : null;
                const connected = (socket.remoteAddress || "").replace(/^::ffff:/, "");
                const host = cameraHost || reported || connected;
                state.board.stream = streamUrl(host, port);

                if (state.board.stream !== loggedStream) {
                    console.log(`Camera stream: ${state.board.stream}` +
                        (cameraHost ? " (CAMERA_HOST)" : reported ? " (reported by the board)" : ""));
                    loggedStream = state.board.stream;
                }
            } else {
                return;
            }

            broadcast();
        } else if (type === "CAMSTAT" && parts.length === 4) {
            const numbers = parseInts(parts.slice(1));

            if (numbers && isInt(numbers[0], 0, 1000) && isInt(numbers[1], 0, 100000)) {
                state.board.cameraStats = { fps: numbers[0], modelMs: numbers[1], ball: numbers[2] === 1 };
                broadcast();
            }
        } else if (type === "LINE" && parts.length === 7) {
            const numbers = parseInts(parts.slice(1));

            if (numbers && numbers.every((v) => isInt(v, 0, MAX_DIMENSION))) {
                const [x1, y1, x2, y2, gx, gy] = numbers;
                state.line = { x1, y1, x2, y2, gx, gy };
                broadcast();
            }
        }
    }

    function handleFrame(header, data) {
        const frame = {
            id: header.id,
            index: header.index,
            width: header.width,
            height: header.height,
            ball: header.ball,
            format: header.format,
            data: data.toString("base64")
        };

        if (header.kind === "LIVE") {
            liveFrame = frame;
            io.emit("live", frame);
        } else if (state.pending && state.pending.id === header.id) {
            replayFrame = frame;
            io.emit("replayFrame", frame);
        }
    }

    io.on("connection", (socket) => {
        socket.emit("update", state);

        if (liveFrame) {
            socket.emit("live", liveFrame);
        }

        if (replayFrame) {
            socket.emit("replayFrame", replayFrame);
        }

        socket.on("simulateGoal", requestReview);

        socket.on("decision", (data) => {
            if (data && typeof data.id === "string") {
                decide(data.id, data.result);
            }
        });

        socket.on("replay", (data) => {
            if (!data || !state.pending) {
                return;
            }

            if (REPLAY_ACTIONS[data.action]) {
                sendToBoard(`REPLAY ${REPLAY_ACTIONS[data.action]}`);
            } else if (data.action === "seek" && state.replay &&
                       isInt(data.frame, 0, state.replay.frames - 1)) {
                sendToBoard(`REPLAY SEEK ${data.frame}`);
            }
        });

        socket.on("calibrate", (line) => {
            const keys = ["x1", "y1", "x2", "y2", "gx", "gy"];

            if (line && keys.every((k) => isInt(line[k], 0, MAX_DIMENSION))) {
                sendToBoard(`LINE ${keys.map((k) => line[k]).join(" ")}`);
            }
        });
    });

    const boardServer = net.createServer((socket) => {
        if (board) {
            board.destroy();
        }

        board = socket;
        state.board.connected = true;
        console.log("BeagleY-AI connected");
        broadcast();

        let buffer = Buffer.alloc(0);
        let expected = null;
        let lastData = Date.now();

        const heartbeat = setInterval(() => {
            if (Date.now() - lastData > BOARD_TIMEOUT_MS) {
                console.log("BeagleY-AI not responding");
                socket.destroy();
            } else {
                socket.write("PING\n");
            }
        }, 2000);

        socket.on("data", (chunk) => {
            lastData = Date.now();
            buffer = Buffer.concat([buffer, chunk]);

            while (true) {
                if (expected) {
                    if (buffer.length < expected.length) {
                        break;
                    }

                    handleFrame(expected, buffer.subarray(0, expected.length));
                    buffer = buffer.subarray(expected.length);
                    expected = null;
                    continue;
                }

                const end = buffer.indexOf(10);

                if (end < 0) {
                    if (buffer.length > 1024) {
                        socket.destroy();
                    }
                    break;
                }

                const line = buffer.subarray(0, end).toString("utf8").trim();
                buffer = buffer.subarray(end + 1);

                const header = parseFrameHeader(line);

                if (header === false) {
                    // The stream cannot be resynchronised after a bad frame header.
                    socket.destroy();
                    return;
                }

                if (header) {
                    expected = header;
                } else {
                    handleBoardLine(line, socket);
                }
            }
        });

        socket.on("close", () => {
            clearInterval(heartbeat);

            if (board === socket) {
                board = null;
                liveFrame = null;
                state.board = { connected: false, state: null, camera: null, ir: null, stream: null, cameraStats: null };
                console.log("BeagleY-AI disconnected");
                broadcast();
            }
        });

        socket.on("error", () => {});
    });

    const listen = (target, port, address) => new Promise((resolve, reject) => {
        target.once("error", reject);
        target.listen(port, address, () => resolve(target.address().port));
    });

    return Promise.all([
        listen(server, httpPort, host),
        listen(boardServer, boardPort, undefined)
    ]).then(([actualHttpPort, actualBoardPort]) => ({
        httpPort: actualHttpPort,
        boardPort: actualBoardPort,
        close() {
            io.close();
            if (board) {
                board.destroy();
            }
            boardServer.close();
            server.close();
        }
    }));
}

module.exports = { startServer, validCameraHost, streamUrl };

if (require.main === module) {
    startServer().then(({ httpPort, boardPort }) => {
        console.log(`True Goal dashboard: http://localhost:${httpPort}`);
        console.log(`Waiting for BeagleY-AI on port ${boardPort}`);
        console.log(process.env.CAMERA_HOST
            ? `Camera host: ${process.env.CAMERA_HOST.trim()} (CAMERA_HOST)`
            : "Camera host: detected from the board (set CAMERA_HOST to override)");
    }).catch((err) => {
        console.error(err.message);
        process.exit(1);
    });
}
