# Test stand-in for goal_cam.py's external interface only: the UDP "goal"
# trigger, the MJPEG stream, the printed lines and the saved replay clips.
# It reads a video file, has no model and does no detection. Never deploy it.
#
#   python3 -u fake_goal_cam.py VIDEO [--seen yes,no] [--web-port N] [--trigger-port N]

import argparse
import os
import socket
import threading
import time
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2

PRE = 4.0
POST = 1.5
COOLDOWN = 3.0

parser = argparse.ArgumentParser()
parser.add_argument("video")
parser.add_argument("--seen", default="yes", help="answers for successive triggers, e.g. yes,no")
parser.add_argument("--web-port", type=int, default=8080)
parser.add_argument("--trigger-port", type=int, default=5005)
args = parser.parse_args()

seen_answers = args.seen.split(",")
lock = threading.Lock()
frames = deque()
latest = None
pending = None
last_trigger = 0.0
triggers = 0


def camera():
    global latest
    cap = cv2.VideoCapture(args.video)
    if not cap.isOpened():
        print("cant open camera", args.video)
        os._exit(1)
    while True:
        ok, f = cap.read()
        if not ok:
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            continue
        now = time.time()
        with lock:
            frames.append((now, f))
            while frames and frames[0][0] < now - (PRE + POST + 1):
                frames.popleft()
            latest = f
        time.sleep(1 / 15.0)


def trigger(why):
    global pending, last_trigger, triggers
    now = time.time()
    with lock:
        if pending is not None or now - last_trigger < COOLDOWN:
            print("goal trigger from %s ignored (too soon after the last one)" % why)
            return
        pending = now
        last_trigger = now
        triggers += 1
    print("GOAL trigger from %s, recording %.1f more seconds" % (why, POST))


def replay():
    global pending
    while True:
        time.sleep(0.05)
        with lock:
            t0 = pending
        if t0 is None or time.time() < t0 + POST:
            continue
        with lock:
            clip = [(t, f) for (t, f) in frames if t0 - PRE <= t <= t0 + POST]
            answer = seen_answers[(triggers - 1) % len(seen_answers)]
        print("GOAL - camera saw the ball: %s (%d frames, 0 ball detections)" % (answer.upper(), len(clip)))
        os.makedirs("replays", exist_ok=True)
        name = "replays/goal_" + datetime.fromtimestamp(t0).strftime("%Y%m%d_%H%M%S")
        h, w = clip[0][1].shape[:2]
        for suffix, label in ((".avi", True), ("_clean.avi", False)):
            out = cv2.VideoWriter(name + suffix, cv2.VideoWriter_fourcc(*"MJPG"), 15.0, (w, h))
            for i, (t, f) in enumerate(clip):
                img = f.copy()
                if label:
                    cv2.putText(img, "REPLAY %+.1f s" % (t - t0), (8, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
                out.write(img)
                time.sleep(0.01)
            out.release()
        print("saved %s.avi (%d frames, 15 fps, normal speed)" % (name, len(clip)))
        with lock:
            pending = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        pass

    def do_GET(self):
        if self.path != "/stream":
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.end_headers()
        try:
            while True:
                with lock:
                    f = latest
                if f is not None:
                    data = cv2.imencode(".jpg", f)[1].tobytes()
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                     + str(len(data)).encode() + b"\r\n\r\n" + data + b"\r\n")
                time.sleep(1 / 15.0)
        except (BrokenPipeError, ConnectionResetError):
            pass


def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", args.trigger_port))
    while True:
        data, _ = s.recvfrom(100)
        if data.strip().lower().startswith(b"goal"):
            trigger("udp message")


for target in (camera, replay, udp):
    threading.Thread(target=target, daemon=True).start()
server = ThreadingHTTPServer(("0.0.0.0", args.web_port), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
print("live view: open  http://127.0.0.1:%d  in the browser on your PC" % args.web_port)

try:
    while True:
        time.sleep(2)
        print("cam 15 fps | model 300 ms, 3.0 runs/s | ball at (320, 240) score 0.90 | viewers 0 | buffer %.1f s"
              % (PRE + POST))
except KeyboardInterrupt:
    print("stopped")
