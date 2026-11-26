# Test stand-in for detect_ball.py (the YOLO model), so the real camera/goal_cam.py can run
# end to end without best.onnx. Never deploy it.
#
# It finds an orange ball by colour and takes about 300 ms per run like the real model.
# The score comes from the file "fake_score" in the current folder:
#   0.9          a ball at its real position with score 0.9 (0 = no ball)
#   static:0.9   a ball that never moves, at (100, 100), with score 0.9
import os
import time

import cv2

CONF = 0.4
infer_ms = 0.0


def find_balls(frame, conf):
    global infer_ms
    start = time.time()
    time.sleep(0.3)
    try:
        with open("fake_score") as f:
            setting = f.read().strip()
    except OSError:
        setting = "0.9"
    infer_ms = (time.time() - start) * 1000
    if setting.startswith("static:"):
        score = float(setting[7:])
        return [((80, 80, 40, 40), score)] if score >= conf else []
    score = float(setting)
    if score < conf:
        return []
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
    mask = cv2.inRange(hsv, (5, 120, 120), (25, 255, 255))
    points = cv2.findNonZero(mask)
    if points is None:
        return []
    x, y, w, h = cv2.boundingRect(points)
    return [((x, y, w, h), score)]


def draw(img, results):
    for (x, y, w, h), score in results:
        cv2.rectangle(img, (x, y), (x + w, y + h), (0, 255, 0), 2)
