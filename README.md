# True Goal

Goal-line assistance for soccer (SFU ENSC 351). IR break-beams on the goal line
detect a possible goal, the BeagleY-AI opens a review with a YOLO camera replay,
and the referee confirms or rejects it from a web dashboard or a rotary encoder.
The referee always makes the final decision: the camera only reports whether it
saw the ball, which does not prove the whole ball crossed the line.

## How it works

```
 IR beams ─┐                                    ┌─ Windows laptop ─────────────────┐
 Encoder ──┤   BeagleY-AI                       │ laptop/server.js (Node.js)       │
 LEDs ─────┤   build/board/true_goal (C++)  ◄──►│  TCP 4000: board link            │
 Buzzer ───┘     │ starts and supervises        │  HTTP 3000: dashboard + camera   │
                 ▼                              │  relay at /camera/stream         │
 USB webcam ► /root/ball/goal_cam.py (YOLO)     └──────────────▲───────────────────┘
               MJPEG :8080, UDP trigger :5005,                 │ browser
               replays/*.avi                          http://localhost:3000
```

1. A broken IR beam that is clear again 1 s later (or **Simulate Event**) opens a review.
2. `true_goal` sends `goal` to goal_cam.py over UDP; goal_cam.py saves a replay clip
   around the trigger and reports whether it saw the ball.
3. The dashboard shows the replay. Step through it with the website controls or the encoder.
4. **Confirm Goal**: green LED for 3 s and a short two-note chime; the goal count goes up.
   **No Goal**: red LED for 3 s, no sound.
5. The system returns to monitoring and live video.

Only one `true_goal` can run at a time (`/tmp/true_goal.lock`), and it is the only
program that starts goal_cam.py, so only one process ever owns the webcam.

## Hardware

BeagleY-AI, USB webcam, three IR break-beam pairs, rotary encoder module with push
button (3.3 V), red and green LEDs with resistors, passive buzzer switched by an NPN
transistor. All signals are 3.3 V.

| Component | GPIO | Header pin |
|---|---|---|
| Encoder CLK / A | GPIO16 | 36 |
| Encoder DT / B | GPIO17 | 11 |
| Encoder SW | GPIO27 | 13 |
| Green LED | GPIO5 | 29 |
| Red LED | GPIO26 | 37 |
| Passive buzzer (via NPN) | GPIO25 | 22 |
| IR sensor 1 | GPIO18 | 12 |
| IR sensor 2 | GPIO23 | 16 |
| IR sensor 3 | GPIO24 | 18 |

Lines are found by name (`gpioinfo` shows them), so gpiochip numbers do not matter.
Pins are defined in `include/pins.h`. Inputs use the internal pull-ups.

## Software needed

- **BeagleY-AI**: CMake 3.16+, g++ with C++17, Python 3 with goal_cam.py and its model
  in `/root/ball` (not in this repository).
- **Laptop**: Node.js 22 or newer (`npm test` uses its built-in WebSocket).

## Find the IP addresses

- Laptop (Windows PowerShell): `ipconfig` - use the IPv4 address of the adapter on
  the same network as the board (Mobile Hotspot is usually `192.168.137.1`).
- BeagleY-AI: `hostname -I`

## Run it

**Laptop** (first time: `cd laptop` then `npm install`):

```powershell
cd laptop
npm start
```

Open http://localhost:3000. The camera address is detected automatically (the board
reports its own IP). To set it by hand, start the server with:

```powershell
$env:CAMERA_HOST = "192.168.137.76"   # the board's IP
npm start
```

**BeagleY-AI** (first time, or after pulling changes):

```bash
cd /root/ENSC-351-Goal-line-Detector
cmake -S . -B build/board -DTRUE_GOAL_MOCK_GPIO=OFF
cmake --build build/board -j4
```

Then, with `LAPTOP_IP` replaced by the laptop's address:

```bash
./build/board/true_goal --server LAPTOP_IP                  # all IR sensors
./build/board/true_goal --server LAPTOP_IP --no-ir          # no IR sensors
./build/board/true_goal --server LAPTOP_IP --skip-ir 18     # without GPIO18 (list: 18,23)
```

`true_goal` starts goal_cam.py itself; do not start it separately. Stop with **Ctrl+C**
(or `kill $(cat /tmp/true_goal.lock)` from another terminal); this releases the GPIO
lines and stops goal_cam.py, which frees the webcam.

## Hardware tests (on the board, with true_goal stopped)

```bash
./build/board/true_goal --test-leds      # green 2 s, red 2 s, then both off
./build/board/true_goal --test-buzzer    # plays the goal chime once
./build/board/true_goal --test-encoder   # prints CLOCKWISE / COUNTERCLOCKWISE / BUTTON PRESSED, Ctrl+C to stop
./build/board/true_goal --check-avi /root/ball/replays/FILE.avi   # a saved replay is readable
```

Camera on its own: `cd /root/ball && python3 -u goal_cam.py`, open
`http://BOARD_IP:8080/stream` in a browser, then Ctrl+C.

The chime notes and duty cycle are at the top of `src/buzzer.cpp` (`kChime`, `kDutyPercent`).

## Automated tests

These use simulated GPIO and a stand-in camera; they do not test real hardware.
Use a separate build folder so the real `build/board/true_goal` is never replaced.

```bash
cmake -S . -B build/mock -DTRUE_GOAL_MOCK_GPIO=ON
cmake --build build/mock -j4
ctest --test-dir build/mock --output-on-failure
```

```powershell
cd laptop
npm test
```

## Troubleshooting

| Problem | Fix |
|---|---|
| `True Goal is already running (PID n)` | Stop the other copy: `kill n`. |
| Dashboard says BeagleY-AI Disconnected | Check `--server` is the laptop's IP and Windows Firewall allows Node.js on port 4000. |
| Black camera panel | Check the server's `Camera stream:` line. If the address is wrong, set `CAMERA_HOST` to the board's IP and restart `npm start`. Test `http://BOARD_IP:8080/stream` directly. |
| `Camera not delivering frames` | Check the webcam (`ls /dev/video*`) and that nothing else uses it: `fuser -v /dev/video*`, `pgrep -af goal_cam.py`. |
| `UDP port 5005 is in use` | A goal_cam.py started by hand is still running: `pkill -f goal_cam.py`. |
| IR status Degraded | One sensor is missing or blocked for over 5 s; the board terminal names it. Fix it or use `--skip-ir`. |
| Encoder direction reversed | Swap `kEncoderA` and `kEncoderB` in `include/pins.h` and rebuild. |
| `GPIO line GPIOxx not found` | Run `gpioinfo` and check the line names; `include/pins.h` also accepts `"gpiochipN:line"`. |

## Known limitations

- The camera reports whether it saw the ball near the trigger, not whether the whole
  ball crossed the line; the referee decides.
- goal_cam.py ignores triggers within 3 s of the previous one; such an event has no replay.
- An IR receiver that is disconnected reads as "clear" (pull-up), so it cannot be
  detected automatically; use `--test-` commands and the board log when setting up.
- The buzzer uses software PWM, so its pitch can wobble slightly under heavy CPU load.
