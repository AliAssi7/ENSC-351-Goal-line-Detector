#ifndef GOALCAM_H
#define GOALCAM_H

#include "camera.h"

#include <functional>
#include <string>
#include <vector>

// Runs the existing YOLO camera program (goal_cam.py) as a child process and
// uses only its documented interface: the UDP "goal" trigger, the MJPEG .avi
// clips it saves in replays/, and the lines it prints. goal_cam.py remains the
// only program that opens the camera.
namespace goalcam {

struct Config {
    std::string dir = "/root/ball";
    std::string command = "python3 -u goal_cam.py";
    int triggerPort = 5005;
    int clipTimeoutMs = 20000;
};

// One YOLO inference run listed by goal_cam.py's EVIDENCE line: its time
// relative to the trigger, its best ball confidence (0 = no ball) and, for a
// ball, the ball's centre in pixels.
struct Inference {
    int dtMs;
    double confidence;
    bool hasPosition = false;
    int x = 0;
    int y = 0;
};

// What the camera saw around one event. result is one of:
//   pending
//   confident     enough runs, and a moving ball in kMinBallRuns of them at kAutoConfidence
//   stationary    a confident ball that does not move (e.g. a spare ball lying in view)
//   low           ball seen, but not confidently enough
//   none          enough runs, no ball
//   insufficient  too few runs, or too little video for a replay
//   seen, missed  goal_cam.py's YES/NO without an EVIDENCE line (unverified)
//   ignored       trigger ignored during goal_cam.py's cooldown
//   unavailable   no answer in time
// The numbers are only filled in from an EVIDENCE line (measured = true).
struct Evidence {
    std::string result = "pending";
    bool measured = false;
    int runs = 0;           // inference runs inside the window
    int ballRuns = 0;       // runs with the ball at or above kAutoConfidence
    double best = 0;        // highest ball confidence inside the window
    int firstMs = 0;        // first and last ball detection, relative to the beam break
    int lastMs = 0;
    int movePx = 0;         // how far the confident ball moved inside the window
};

// Judges the inference runs inside the window around the beam break.
// breakOffsetMs: how long before the trigger the beam broke (0 for manual).
Evidence evaluate(const std::vector<Inference>& runs, int breakOffsetMs);

// Parses "EVIDENCE <item> ..." where an item is dtMs:confidence or
// dtMs:confidence:x:y. Returns false if malformed.
bool parseEvidence(const std::string& line, std::vector<Inference>& runs);

// replay: pending or unavailable; a delivered clip is reported through clip.
struct Callbacks {
    std::function<void(const std::string& status)> status;   // starting, yolo, offline
    std::function<void(const std::string& id, const Evidence& evidence,
                       const std::string& replay)> evidence;
    std::function<void(const std::string& id, std::vector<camera::FramePtr> frames, int fps)> clip;
    std::function<void(double fps, double modelMs, bool ball)> stats;
};

void start(const Config& config, const Callbacks& callbacks);

// Asks goal_cam.py to keep a replay for this event and report its evidence.
bool capture(const std::string& eventId, int breakOffsetMs);

void stop();

// Reads the JPEG frames of a finished MJPEG .avi. Returns false while the
// file is still being written or if it is not MJPEG.
bool readAvi(const std::string& path, std::vector<camera::FramePtr>& frames, int& fps);

}

#endif
