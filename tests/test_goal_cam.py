# Tests the evidence code of the real camera/goal_cam.py without a camera or the YOLO model.
#   python3 tests/test_goal_cam.py
import contextlib
import importlib.util
import io
import os
import re
import sys
import tempfile
import threading
import time
import types
import unittest

# A stand-in for detect_ball.py: find_balls() returns whatever the test sets.
fake_db = types.ModuleType("detect_ball")
fake_db.CONF = 0.4
fake_db.infer_ms = 300.0
fake_db.next_result = []
fake_db.calls = 0
fake_db.draw = lambda img, results: None


def find_balls(frame, conf):
    fake_db.calls += 1
    if isinstance(fake_db.next_result, Exception):
        raise fake_db.next_result
    return fake_db.next_result


fake_db.find_balls = find_balls
sys.modules["detect_ball"] = fake_db
REAL_CV2 = True
for name in ("cv2", "numpy"):
    try:
        __import__(name)
    except ImportError:
        sys.modules[name] = types.ModuleType(name)
        REAL_CV2 = False

path = os.path.join(os.path.dirname(__file__), "..", "camera", "goal_cam.py")
spec = importlib.util.spec_from_file_location("goal_cam", path)
gc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gc)
real_save_clip = gc.save_clip
gc.save_clip = lambda clip: None          # no video writing in tests

# The grammar of true_goal's parser (src/goalcam.cpp, parseEvidence).
EVIDENCE = re.compile(r"^EVIDENCE( -?\d+:[01]\.\d{3}(:\d+:\d+)?)*$")


class FakeCamera:
    def __init__(self):
        self.lock = threading.Lock()
        self.latest = None
        self.count = 0
        self.buf = []

    def show(self, t):
        with self.lock:
            self.latest = (t, "frame")
            self.count += 1


def wait_for(check, timeout=3.0):
    end = time.time() + timeout
    while time.time() < end:
        if check():
            return True
        time.sleep(0.01)
    return False


class EvidenceLine(unittest.TestCase):
    T0 = 1000.0

    def line(self, runs):
        return gc.evidence_line(runs, self.T0)

    def test_format_matches_true_goal_parser(self):
        line = self.line([(999.0, 0.0, None, None), (998.5, 0.9123, 320, 241), (1001.2, 0.0, None, None)])
        self.assertEqual(line, "EVIDENCE -1500:0.912:320:241 -1000:0.000 1200:0.000")
        self.assertRegex(line, EVIDENCE)
        self.assertEqual(self.line([]), "EVIDENCE")

    def test_window_edges(self):
        runs = [(self.T0 - gc.PRE - 0.001, 0.9, 1, 1), (self.T0 - gc.PRE, 0.9, 2, 2),
                (self.T0 + gc.POST, 0.9, 3, 3), (self.T0 + gc.POST + 0.001, 0.9, 4, 4)]
        self.assertEqual(self.line(runs), "EVIDENCE -4000:0.900:2:2 1500:0.900:3:3")

    def test_stale_runs_from_earlier_are_left_out(self):
        runs = [(self.T0 - 30, 0.99, 5, 5), (self.T0 - 10, 0.99, 6, 6), (self.T0 - 1, 0.0, None, None)]
        self.assertEqual(self.line(runs), "EVIDENCE -1000:0.000")

    def test_same_frame_counted_once(self):
        runs = [(999.0, 0.9, 10, 10), (999.0, 0.9, 10, 10)]
        self.assertEqual(self.line(runs), "EVIDENCE -1000:0.900:10:10")

    def test_score_never_rounds_up_past_a_threshold(self):
        self.assertEqual(self.line([(999.0, 0.7499, 1, 1)]), "EVIDENCE -1000:0.749:1:1")
        self.assertEqual(self.line([(999.0, 1.0, 1, 1)]), "EVIDENCE -1000:1.000:1:1")


class DetectorRuns(unittest.TestCase):
    def test_every_finished_run_once_and_no_failed_runs(self):
        cam = FakeCamera()
        det = gc.Detector(cam)
        try:
            fake_db.next_result = [((300, 200, 40, 40), 0.87), ((10, 10, 20, 20), 0.5)]
            cam.show(100.0)
            self.assertTrue(wait_for(lambda: det.runs == 1))

            # The same frame again is not run again.
            calls = fake_db.calls
            time.sleep(0.1)
            self.assertEqual(fake_db.calls, calls)

            fake_db.next_result = []
            cam.show(100.3)
            self.assertTrue(wait_for(lambda: det.runs == 2))

            # A failed run is not recorded as "no ball".
            fake_db.next_result = RuntimeError("model failed")
            with contextlib.redirect_stderr(io.StringIO()):
                cam.show(100.6)
                self.assertTrue(wait_for(lambda: fake_db.calls == calls + 2))
                time.sleep(0.05)
            self.assertEqual(det.runs, 2)

            with det.lock:
                log = list(det.runs_log)
                hist = list(det.hist)
            self.assertEqual(log, [(100.0, 0.87, 320, 220), (100.3, 0.0, None, None)])
            self.assertEqual(hist, [(100.0, 320, 220, 0.87)])   # the replay drawing is unchanged
        finally:
            det.alive = False
            fake_db.next_result = []
            time.sleep(1.1)


class ReplayEvidence(unittest.TestCase):
    def build(self, why, frames=True):
        cam = FakeCamera()
        t0 = 2000.0
        if frames:
            cam.buf = [(t0 - 1.0, "f"), (t0, "f"), (t0 + 1.0, "f")]
        det = types.SimpleNamespace(lock=threading.Lock(), hist=[], runs_log=[
            (t0 - 20.0, 0.99, 50, 50),          # long before: belongs to nobody
            (t0 - 1.5, 0.91, 200, 240),
            (t0 - 1.2, 0.88, 290, 250),
            (t0 - 0.9, 0.0, None, None),
        ])
        rep = gc.Replay(cam, det)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rep.build(t0, why)
        return out.getvalue().splitlines()

    def test_udp_trigger_prints_evidence_before_yes_no(self):
        lines = self.build("udp message")
        self.assertEqual(lines[0], "EVIDENCE -1500:0.910:200:240 -1200:0.880:290:250 -900:0.000")
        self.assertTrue(lines[1].startswith("GOAL - camera saw the ball: "))

    def test_web_button_never_supplies_evidence(self):
        lines = self.build("web button")
        self.assertFalse(any(l.startswith("EVIDENCE") for l in lines))
        self.assertTrue(lines[0].startswith("GOAL - camera saw the ball: "))

    def test_not_enough_video_prints_no_evidence(self):
        lines = self.build("udp message", frames=False)
        self.assertEqual(lines, ["not enough video in the buffer for a replay"])

    def test_trigger_remembers_its_source(self):
        rep = gc.Replay(FakeCamera(), types.SimpleNamespace(lock=threading.Lock(), hist=[], runs_log=[]))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(rep.trigger("web button"))
            self.assertFalse(rep.trigger("udp message"))   # busy: true_goal sees "ignored"
        self.assertEqual(rep.pending_why, "web button")


@unittest.skipUnless(REAL_CV2, "needs OpenCV")
class BoardFeaturesKept(unittest.TestCase):
    # The board's replay files and photos still work after the evidence changes.
    def test_annotated_and_clean_clips_and_photos(self):
        import numpy as np
        old = os.getcwd()
        with tempfile.TemporaryDirectory() as tmp:
            os.chdir(tmp)
            try:
                frames = [(100.0 + i / 15, np.zeros((48, 64, 3), np.uint8)) for i in range(5)]
                clip = dict(frames=frames, times=[t for t, f in frames], dets=[(100.1, 10, 10, 0.9)],
                            text="GOAL - camera saw the ball: YES", t0=100.2)
                with contextlib.redirect_stdout(io.StringIO()):
                    real_save_clip(clip)
                names = sorted(os.listdir(gc.SAVE_DIR))
                self.assertEqual(len(names), 2)
                self.assertTrue(names[0].endswith(".avi") and names[1].endswith("_clean.avi"))

                gc.cam = FakeCamera()
                gc.cam.latest = (100.0, np.zeros((48, 64, 3), np.uint8))
                self.assertTrue(os.path.exists(gc.save_photo()))
                self.assertEqual(gc.photos_saved, 1)
            finally:
                os.chdir(old)


if __name__ == "__main__":
    unittest.main(verbosity=2)
