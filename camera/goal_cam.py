# goal_cam.py - live view in the browser + ball detection + instant replay, all in one program
#
# run on the board (in the folder with best.onnx, detect_ball.py and this file):
#   python3 goal_cam.py              -> camera 2, web page on port 8080
#   python3 goal_cam.py 2            -> camera number 2
#   python3 goal_cam.py test.avi     -> use a video file instead of a camera (for testing)
#   python3 goal_cam.py goal         -> (from a second window) pretend the IR sensor fired
#
# then open  http://<board-ip>:8080  in the browser on your PC
#
# how the replay gets triggered: your IR sensor program sends the 4 letters "goal" as a udp packet
# to 127.0.0.1 port 5005 (see send_goal.c). the web page also has a test button.
# when the trigger comes, we wait POST seconds, then play the last PRE+POST seconds in slow motion
# on the web page, and save the clip into the replays folder as an .avi (plus a _clean.avi without drawings)
#
# the page also has a Save photo button and an auto-save switch. photos go into the photos folder,
# they are clean pictures (no boxes) so you can use them to train a better model
#
# for a udp trigger it also prints an EVIDENCE line (before the YES/NO line) listing every model
# run in the replay window, so true_goal can decide whether the goal is certain enough to award
# automatically. see evidence_line() below.

import sys
import math
import os
import time
import socket
import threading
import traceback
from bisect import bisect_right
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2
import numpy as np

import detect_ball as db      # loads best.onnx and gives us find_balls() and draw()

# ---- settings, change these if you want ----
PRE = 4.0             # seconds of video before the goal that go into the replay
POST = 1.5            # seconds of video after the goal
SLOWMO = 0.5          # replay speed, 0.5 = half speed
WEB_PORT = 8080
TRIGGER_PORT = 5005   # udp port the IR sensor program sends "goal" to
COOLDOWN = 3.0        # a second trigger this soon after the first one is ignored (sensor bounce)
STREAM_FPS = 15       # pictures per second sent to the browser
JPEG_QUALITY = 70
SAVE_DIR = "replays"
PHOTO_DIR = "photos"         # photos from the Save photo button and auto-save go here
AUTO_PHOTO_EVERY = 1.0       # seconds between photos when auto-save is on
CONF = db.CONF        # min score for a ball (0.4 unless you changed it in detect_ball.py)

cam = None
det = None
replay = None
viewers = 0
viewers_lock = threading.Lock()
photos_saved = 0              # how many photos are in the photos folder
auto_photo = False            # true while auto-save is switched on
photo_lock = threading.Lock()


class Camera:
    # reads the camera as fast as it can and keeps the last few seconds of frames in memory
    def __init__(self, source):
        self.is_file = not isinstance(source, int)
        if self.is_file:
            self.cap = cv2.VideoCapture(source)
        else:
            self.cap = cv2.VideoCapture(source, cv2.CAP_V4L2)
            self.cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc("M", "J", "P", "G"))
            self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
            self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
            self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        self.ok = self.cap.isOpened()
        self.lock = threading.Lock()
        self.buf = deque()       # (time, frame) for the last PRE+POST+1 seconds
        self.latest = None       # (time, frame) newest frame
        self.count = 0
        self.fps = 0.0
        self.alive = True
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        keep = PRE + POST + 1.0
        n = 0
        t_fps = time.time()
        fails = 0
        while self.alive:
            ok, f = self.cap.read()
            if not ok:
                fails += 1
                if self.is_file and fails < 3:
                    self.cap.set(cv2.CAP_PROP_POS_FRAMES, 0)      # video file ended, start again
                    continue
                print("camera stopped giving frames")
                self.alive = False
                break
            fails = 0
            now = time.time()
            f = f.copy()          # own copy, so nothing can change a frame after it is stored
            with self.lock:
                self.buf.append((now, f))
                while self.buf and self.buf[0][0] < now - keep:
                    self.buf.popleft()
                self.latest = (now, f)
                self.count += 1
            n += 1
            if now - t_fps >= 2:
                self.fps = n / (now - t_fps)
                n = 0
                t_fps = now
            if self.is_file:
                time.sleep(1 / 30.0)      # a video file would play way too fast otherwise


class Detector:
    # runs the model on the newest frame again and again (about 3 times a second on the cpu)
    def __init__(self, camera):
        self.cam = camera
        self.last = None                 # (frame time, results) of the newest detection
        self.hist = deque(maxlen=300)    # (frame time, x, y, score) of the best ball in each run
        self.runs_log = deque(maxlen=300)  # (frame time, score, x, y) of every finished run; 0, None, None = no ball
        self.lock = threading.Lock()
        self.runs = 0
        self.ms = 0.0
        self.alive = True
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        last_count = 0
        while self.alive:
            with self.cam.lock:
                item = self.cam.latest
                count = self.cam.count
            if item is None or count == last_count:
                time.sleep(0.005)
                continue
            last_count = count
            t, frame = item
            try:
                results = db.find_balls(frame, CONF)
            except Exception:
                traceback.print_exc()
                time.sleep(1)
                continue
            self.ms = db.infer_ms
            self.runs += 1
            self.last = (t, results)
            # only runs that finished get here, each on a different frame
            with self.lock:
                if results:
                    (x, y, w, h), score = max(results, key=lambda r: r[1])
                    self.hist.append((t, x + w // 2, y + h // 2, score))
                    self.runs_log.append((t, score, x + w // 2, y + h // 2))
                else:
                    self.runs_log.append((t, 0.0, None, None))


class Replay:
    # waits for a goal trigger, then builds the clip
    def __init__(self, camera, detector):
        self.cam = camera
        self.det = detector
        self.lock = threading.Lock()
        self.pending = None       # time of the goal while we wait for the POST seconds
        self.pending_why = None   # what sent it: "udp message" (true_goal) or "web button"
        self.last_trigger = 0.0
        self.clip = None          # the clip that is playing on the web page right now
        threading.Thread(target=self.loop, daemon=True).start()

    def trigger(self, why):
        now = time.time()
        with self.lock:
            if self.pending is not None or now - self.last_trigger < COOLDOWN:
                print("goal trigger from %s ignored (too soon after the last one)" % why)
                return False
            self.pending = now
            self.pending_why = why
            self.last_trigger = now
        print("GOAL trigger from %s, recording %.1f more seconds" % (why, POST))
        return True

    def loop(self):
        while True:
            time.sleep(0.05)
            with self.lock:
                t0 = self.pending
                why = self.pending_why
            if t0 is None or time.time() < t0 + POST:
                continue
            try:
                self.build(t0, why)
            except Exception:
                traceback.print_exc()
            with self.lock:
                self.pending = None

    def build(self, t0, why="udp message"):
        with self.cam.lock:
            frames = [(t, f) for (t, f) in self.cam.buf if t0 - PRE <= t <= t0 + POST]
        if len(frames) < 2:
            print("not enough video in the buffer for a replay")
            return
        with self.det.lock:
            dets = [d for d in self.det.hist if t0 - PRE <= d[0] <= t0 + POST]
            runs = list(self.det.runs_log)
        # only true_goal's udp trigger gets evidence, so the web page's test button can never
        # supply it for an IR event. it is printed before the YES/NO line, which true_goal needs.
        if why == "udp message":
            print(evidence_line(runs, t0), flush=True)
        # the camera counts as "saw the ball" if the ai found it shortly before or after the trigger
        seen = any(t0 - 2.0 <= d[0] <= t0 + POST for d in dets)
        text = "GOAL - camera saw the ball: " + ("YES" if seen else "NO")
        clip = dict(frames=frames, times=[t for t, f in frames], dets=dets, text=text, t0=t0)
        print(text, "(%d frames, %d ball detections)" % (len(frames), len(dets)))
        clip["start"] = time.time()
        self.clip = clip          # the web page starts playing it right away
        threading.Thread(target=save_clip, args=(clip,), daemon=True).start()


def evidence_line(runs, t0):
    # "EVIDENCE" and one item per finished model run in the replay window, oldest first:
    #   ms:score        no ball (score 0)
    #   ms:score:x:y    best ball, with its centre in pixels
    # ms is from the trigger t0 (negative = before it). true_goal shifts it to the moment the
    # IR beam broke and judges the runs around that moment.
    items = []
    used = set()
    for t, score, x, y in sorted(runs, key=lambda r: r[0]):
        if not (t0 - PRE <= t <= t0 + POST) or t in used:
            continue
        used.add(t)
        ms = int(round((t - t0) * 1000))
        score = math.floor(score * 1000) / 1000      # never round up past a threshold
        if x is None or score <= 0:
            items.append("%d:%.3f" % (ms, score))
        else:
            items.append("%d:%.3f:%d:%d" % (ms, score, x, y))
    return " ".join(["EVIDENCE"] + items)


def draw_replay(frame, clip, ts, label):
    # a copy of the frame with the ball path so far, a banner on top and the time from the goal
    img = frame.copy()
    pts = [(int(x), int(y)) for (t, x, y, s) in clip["dets"] if t <= ts]
    if len(pts) > 1:
        cv2.polylines(img, [np.array(pts, np.int32)], False, (0, 255, 255), 2)
    for p in pts:
        cv2.circle(img, p, 6, (0, 255, 255), -1)
    w = img.shape[1]
    h = img.shape[0]
    cv2.rectangle(img, (0, 0), (w, 30), (0, 0, 0), -1)
    cv2.putText(img, label + "  |  " + clip["text"], (8, 21), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1)
    cv2.putText(img, "%+.1f s" % (ts - clip["t0"]), (8, h - 12), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2)
    return img


def save_clip(clip):
    try:
        os.makedirs(SAVE_DIR, exist_ok=True)
        name = os.path.join(SAVE_DIR, "goal_" + datetime.fromtimestamp(clip["t0"]).strftime("%Y%m%d_%H%M%S") + ".avi")
        times = clip["times"]
        fps = (len(times) - 1) / (times[-1] - times[0]) if times[-1] > times[0] else 30.0
        h, w = clip["frames"][0][1].shape[:2]
        # two files: one with the banner and ball path drawn on it, one clean (the clean one is for training photos)
        clean_name = name[:-4] + "_clean.avi"
        fourcc = cv2.VideoWriter_fourcc("M", "J", "P", "G")
        out = cv2.VideoWriter(name, fourcc, fps, (w, h))
        clean = cv2.VideoWriter(clean_name, fourcc, fps, (w, h))
        if not out.isOpened():
            print("cant write", name)
            return
        for t, f in clip["frames"]:
            out.write(draw_replay(f, clip, t, "REPLAY"))
            if clean.isOpened():
                clean.write(f)
        out.release()
        clean.release()
        print("saved %s and %s (%d frames, %.0f fps, normal speed)" % (name, clean_name, len(times), fps))
    except Exception:
        traceback.print_exc()


def save_photo():
    # saves the newest camera picture as a clean jpg (no boxes drawn on it), for training
    global photos_saved
    with cam.lock:
        item = cam.latest
    if item is None:
        return None
    t, f = item
    os.makedirs(PHOTO_DIR, exist_ok=True)
    stamp = datetime.fromtimestamp(t).strftime("%Y%m%d_%H%M%S_%f")[:-3]     # down to milliseconds
    name = os.path.join(PHOTO_DIR, "cam_" + stamp + ".jpg")
    if os.path.exists(name):
        return name                   # same camera frame as the last photo, dont count it twice
    cv2.imwrite(name, f, [cv2.IMWRITE_JPEG_QUALITY, 95])
    with photo_lock:
        photos_saved += 1
    return name


def auto_photo_loop():
    last = 0.0
    while True:
        time.sleep(0.1)
        if auto_photo and time.time() - last >= AUTO_PHOTO_EVERY:
            save_photo()
            last = time.time()


def current_picture():
    # the picture to show right now: the replay if one is playing, otherwise the live camera with boxes
    clip = replay.clip
    if clip is not None:
        times = clip["times"]
        rt = times[0] + (time.time() - clip["start"]) * SLOWMO      # where we are in the clip, in real time
        if rt > times[-1] + 0.5:
            if replay.clip is clip:
                replay.clip = None                                    # replay is over, back to live
        else:
            i = max(0, min(bisect_right(times, rt) - 1, len(times) - 1))
            t, f = clip["frames"][i]
            return draw_replay(f, clip, t, "REPLAY %gx" % SLOWMO)

    with cam.lock:
        item = cam.latest
    if item is None:
        return None
    t, f = item
    img = f.copy()                  # always draw on a copy, the original is in the replay buffer
    last = det.last
    ball = False
    if last is not None and time.time() - last[0] < 0.7:
        # the model needs about 300 ms, so this box can trail a fast ball a little
        db.draw(img, last[1])
        ball = len(last[1]) > 0
    text = "LIVE | cam %.0f fps | model %.0f ms | ball: %s | photos %d" % (
        cam.fps, det.ms, "yes" if ball else "no", photos_saved)
    if auto_photo:
        text += " | AUTO-SAVE ON"
    cv2.rectangle(img, (0, img.shape[0] - 26), (img.shape[1], img.shape[0]), (0, 0, 0), -1)
    cv2.putText(img, text, (8, img.shape[0] - 8), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1)
    return img


PAGE = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>Goal cam</title>
<style>
body { background:#111; color:#eee; font-family:sans-serif; text-align:center; margin:0; padding:12px; }
img { max-width:100%; border:2px solid #444; }
button { font-size:20px; padding:10px 24px; margin:12px; }
</style></head>
<body>
<h2>Goal cam</h2>
<img src="/stream"><br>
<button onclick="call('/trigger')">Test GOAL replay</button>
<button onclick="call('/photo')">Save photo</button>
<button onclick="call('/auto')">Auto-save photos on/off</button>
<div id="msg"></div>
<script>
function call(url) {
  fetch(url).then(r => r.text()).then(t => { document.getElementById('msg').textContent = t })
}
</script>
</body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass                          # keep the terminal quiet

    def reply(self, body, ctype="text/plain", code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        global auto_photo
        if self.path == "/":
            self.reply(PAGE, "text/html")
        elif self.path == "/stream":
            self.stream()
        elif self.path == "/trigger":
            ok = replay.trigger("web button")
            self.reply(b"goal sent, replay in a moment" if ok else b"too soon after the last goal, wait a few seconds")
        elif self.path == "/photo":
            name = save_photo()
            if name:
                self.reply(("photo saved, %d photos in the photos folder" % photos_saved).encode())
            else:
                self.reply(b"no camera picture yet")
        elif self.path == "/auto":
            auto_photo = not auto_photo
            if auto_photo:
                self.reply(("auto-save is ON, one photo every %g s" % AUTO_PHOTO_EVERY).encode())
            else:
                self.reply(("auto-save is OFF, %d photos in the photos folder" % photos_saved).encode())
        else:
            self.reply(b"not found", code=404)

    def stream(self):
        global viewers
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        with viewers_lock:
            viewers += 1
        try:
            while True:
                t = time.time()
                img = current_picture()
                if img is not None:
                    ok, jpg = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
                    data = jpg.tobytes()
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                     + str(len(data)).encode() + b"\r\n\r\n" + data + b"\r\n")
                wait = 1.0 / STREAM_FPS - (time.time() - t)
                if wait > 0:
                    time.sleep(wait)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass                      # the browser tab was closed
        finally:
            with viewers_lock:
                viewers -= 1


def udp_listener():
    # the IR sensor program sends "goal" here (127.0.0.1 = only programs on this same board)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", TRIGGER_PORT))
    while True:
        data, addr = s.recvfrom(100)
        if data.strip().lower().startswith(b"goal"):
            replay.trigger("udp message")


def my_ip():
    # finds the board's wifi address without sending anything
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "<board-ip>"


def main():
    global cam, det, replay, photos_saved
    args = sys.argv[1:]

    if args and args[0] == "goal":
        # test helper: acts like the IR sensor program
        socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(b"goal", ("127.0.0.1", TRIGGER_PORT))
        print("sent goal")
        return

    source = 2
    if args:
        source = int(args[0]) if args[0].isdigit() else args[0]

    cam = Camera(source)
    if not cam.ok:
        print("cant open camera", source)
        return
    det = Detector(cam)
    replay = Replay(cam, det)
    if os.path.isdir(PHOTO_DIR):
        photos_saved = len([n for n in os.listdir(PHOTO_DIR) if n.lower().endswith(".jpg")])
    threading.Thread(target=auto_photo_loop, daemon=True).start()

    try:
        threading.Thread(target=udp_listener, daemon=True).start()
        server = ThreadingHTTPServer(("0.0.0.0", WEB_PORT), Handler)
    except OSError as e:
        print("cant start (is goal_cam.py already running in another window?):", e)
        return
    threading.Thread(target=server.serve_forever, daemon=True).start()

    print("live view: open  http://%s:%d  in the browser on your PC" % (my_ip(), WEB_PORT))
    print("replay trigger: send the text goal as udp to 127.0.0.1:%d (or press the button on the page)" % TRIGGER_PORT)
    print("press ctrl+c to stop")

    last_runs = 0
    last_time = time.time()
    try:
        while cam.alive:
            time.sleep(2)
            now = time.time()
            runs = det.runs
            rate = (runs - last_runs) / (now - last_time)
            last_runs = runs
            last_time = now
            with cam.lock:
                span = cam.buf[-1][0] - cam.buf[0][0] if len(cam.buf) > 1 else 0.0
            last = det.last
            if last is not None and last[1] and now - last[0] < 1.0:
                (x, y, w, h), score = max(last[1], key=lambda r: r[1])
                ball = "ball at (%d, %d) score %.2f" % (x + w // 2, y + h // 2, score)
            else:
                ball = "no ball"
            print("cam %.0f fps | model %.0f ms, %.1f runs/s | %s | viewers %d | buffer %.1f s | photos %d" % (
                cam.fps, det.ms, rate, ball, viewers, span, photos_saved))
    except KeyboardInterrupt:
        print("stopped")
    cam.alive = False
    det.alive = False


if __name__ == "__main__":
    main()
