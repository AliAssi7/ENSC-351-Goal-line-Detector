# True Goal

Goal-line assistance for soccer (SFU ENSC 351). IR break-beams on the goal line
detect a possible goal and a YOLO camera checks for a soccer ball at that moment.
When both agree confidently the goal is awarded automatically; otherwise the
referee decides from a web dashboard with a replay and a rotary encoder.
This is sensor fusion, not proof that the whole ball crossed the line, and the
referee can overturn an automatic goal.

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

1. A broken IR beam that is clear again 1 s later opens an event (**Evaluating Evidence**).
2. `true_goal` sends `goal` to goal_cam.py over UDP; goal_cam.py saves a replay clip
   and reports what the model saw around the trigger.
3. The decision follows the table below. **Awaiting Decision** lights the yellow LED
   until the referee clicks **Confirm Goal** or **No Goal**, however long that takes.
4. The replay can be stepped through with the website controls or the encoder.
5. The system returns to monitoring and live video.

| IR trigger | Camera evidence | Result |
|---|---|---|
| Yes | A moving ball at ≥ 75 % confidence in ≥ 2 model runs near the beam break | **Automatic Goal** |
| Yes | No ball, low confidence, ball not moving, too little footage, camera busy/unavailable, no answer in 8 s | **Awaiting Decision** |
| No | Ball seen | Keeps monitoring |
| Simulate Event | Anything | **Awaiting Decision** (never automatic) |

A missing ball is never an automatic No Goal. The thresholds are starting values,
not measured accuracy; they are constants at the top of `src/goalcam.cpp`:
`kAutoConfidence` (0.75), `kMinBallRuns` (2), `kMinRuns` (3 model runs in the window),
`kMinBallMovePx` (40: a ball lying still elsewhere in view is not the one that broke
the beam), and the window from 1.5 s before to 1 s after the beam break.

| State | Green | Yellow | Red | Buzzer |
|---|---|---|---|---|
| Monitoring, Evaluating Evidence | off | off | off | silent |
| Awaiting Decision (incl. Simulate Event) | off | **on until decided** | off | silent |
| Goal (automatic or referee) | **3 s** | off | off | whistle once |
| No Goal (referee, or a corrected automatic goal) | off | off | **3 s** | silent |

After an automatic goal, the review card offers **Review Replay** (the goal's saved
clip, with the encoder) and **Correct to No Goal** (same history entry, one goal less,
red LED, no sound).

Only one `true_goal` can run at a time (`/tmp/true_goal.lock`), and it is the only
program that starts goal_cam.py, so only one process ever owns the webcam.

## Hardware

BeagleY-AI, USB webcam, three IR break-beam pairs, rotary encoder module with push
button (3.3 V), green, yellow and red LEDs (each through a 330 Ω resistor to its
GPIO, cathode to common ground), passive buzzer switched by an NPN transistor.
All signals are 3.3 V.

| Component | GPIO | Header pin |
|---|---|---|
| Encoder CLK / A | GPIO16 | 36 |
| Encoder DT / B | GPIO17 | 11 |
| Encoder SW | GPIO27 | 13 |
| Green LED | GPIO5 | 29 |
| Yellow LED | GPIO6 | 31 |
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
./build/board/true_goal --server LAPTOP_IP --skip-ir 18     # the two working sensors (GPIO23, GPIO24)
./build/board/true_goal --server LAPTOP_IP                  # all IR sensors
./build/board/true_goal --server LAPTOP_IP --no-ir          # manual testing: Simulate Event only
```

`true_goal` starts goal_cam.py itself; do not start it separately. Stop with **Ctrl+C**
(or `kill $(cat /tmp/true_goal.lock)` from another terminal); this releases the GPIO
lines and stops goal_cam.py, which frees the webcam.

## Hardware tests (on the board, with true_goal stopped)

```bash
./build/board/true_goal --test-leds      # green, yellow, red for 2 s each, then all off
./build/board/true_goal --test-buzzer    # plays the goal whistle once
./build/board/true_goal --test-encoder   # prints CLOCKWISE / COUNTERCLOCKWISE / BUTTON PRESSED, Ctrl+C to stop
./build/board/true_goal --check-avi /root/ball/replays/FILE.avi   # a saved replay is readable
```

Camera on its own: `cd /root/ball && python3 -u goal_cam.py`, open
`http://BOARD_IP:8080/stream` in a browser, then Ctrl+C.

### Buzzer

The goal notification is a referee's full-time whistle: **PIP - PIP - PIIIIIP**
(200 ms on, 150 off, 200 on, 150 off, 600 on; about 1.3 s), all at one pitch.
Every setting is at the top of `src/buzzer.cpp`:

- Passive buzzer (default): `kFrequencyHz = 2000`, `kDutyPercent = 25`. Raise the duty
  cycle if it is too quiet; very low duty cycles make it go silent.
- Active buzzer: set `kActiveBuzzer = true`; the pin is just held high for each blast.

## Camera program (camera/goal_cam.py)

`camera/goal_cam.py` is the YOLO camera program. It runs on the board from `/root/ball`,
next to `detect_ball.py` and `best.onnx` (not in this repository), and `true_goal`
starts it. Pulling the repository does **not** update `/root/ball`; after a pull, copy it:

```bash
cd /root/ENSC-351-Goal-line-Detector
sha256sum /root/ball/goal_cam.py      # the version camera/goal_cam.py was built from:
                                      # eb54ab3fbf622ee51c88c2460ffcebe9873b06183fea97027987ecd69b2b6657
cp -n /root/ball/goal_cam.py /root/ball/goal_cam.py.bak     # keeps an existing backup
cp camera/goal_cam.py /root/ball/goal_cam.py
sha256sum /root/ball/goal_cam.py      # now 189f2a4bbd42c75b1d3f67994aa96cf98e3482849bec70472afd31c32af04adc
```

If the first checksum is different, the board's goal_cam.py has changed since: do not
copy; merge the EVIDENCE changes into it by hand (`diff` against `camera/goal_cam.py`).
To undo: `cp /root/ball/goal_cam.py.bak /root/ball/goal_cam.py`.

For each `true_goal` trigger it prints, before its `camera saw the ball: YES/NO` line,
one item per finished model run in the replay window (4 s before to 1.5 s after the
trigger), oldest first:

```
EVIDENCE -2462:0.000 -2180:0.903:224:240 -1843:0.911:256:240 ... 18:0.000
```

`ms:score` is a run that found no ball; `ms:score:x:y` gives the best ball's score and
centre. `ms` is from the moment goal_cam.py received the trigger. `true_goal` knows how
long before that the beam broke (about 1 s, the IR check) and judges only the runs from
1.5 s before to 1 s after the break. Without this line (an older goal_cam.py) every IR
event goes to the referee as "unverified". The web page's own test button never prints
evidence.

## Automated tests

These use simulated GPIO and a stand-in camera; they do not test real hardware.
Use a separate build folder so the real `build/board/true_goal` is never replaced.

```bash
cmake -S . -B build/mock -DTRUE_GOAL_MOCK_GPIO=ON
cmake --build build/mock -j4
ctest --test-dir build/mock --output-on-failure   # C++ tests and python3 tests/test_goal_cam.py
```

`tests/fake_detect_ball.py` can stand in for `detect_ball.py` to run the real
goal_cam.py on a video file without the model (test machines only).

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
| Every IR event says "unverified" | `/root/ball/goal_cam.py` is an older copy without the EVIDENCE line: copy `camera/goal_cam.py` (see above). |
| Yellow LED never lights | Run `--test-leds`; check the LED on GPIO6 / pin 31 and its polarity. |
| Encoder direction reversed | Swap `kEncoderA` and `kEncoderB` in `include/pins.h` and rebuild. |
| `GPIO line GPIOxx not found` | Run `gpioinfo` and check the line names; `include/pins.h` also accepts `"gpiochipN:line"`. |

## Known limitations

- An automatic goal means the IR beam broke and the model saw a moving soccer ball
  around that moment. There is no calibrated goal-line region in the camera image, so
  it does not show that the whole ball crossed the line, or that the ball seen is the
  one that broke the beam (only that it was moving); the referee can correct it.
  The thresholds have not been validated against real play.
- The model runs about 3 times a second on the board, so a fast ball may appear in
  too few runs and go to the referee instead.
- goal_cam.py ignores triggers within 3 s of the previous one; such an event has no replay.
- An IR receiver that is disconnected reads as "clear" (pull-up), so it cannot be
  detected automatically; use `--test-` commands and the board log when setting up.
- The buzzer uses software PWM, so its pitch can wobble slightly under heavy CPU load.
