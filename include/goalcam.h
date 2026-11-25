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

// evidence: pending, seen, missed, ignored or unavailable (no YES/NO result).
// replay: pending or unavailable; a delivered clip is reported through clip.
struct Callbacks {
    std::function<void(const std::string& status)> status;   // starting, yolo, offline
    std::function<void(const std::string& id, const std::string& evidence,
                       const std::string& replay)> evidence;
    std::function<void(const std::string& id, std::vector<camera::FramePtr> frames, int fps)> clip;
    std::function<void(double fps, double modelMs, bool ball)> stats;
};

void start(const Config& config, const Callbacks& callbacks);

// Asks goal_cam.py to keep a replay for this event.
bool capture(const std::string& eventId);

void stop();

// Reads the JPEG frames of a finished MJPEG .avi. Returns false while the
// file is still being written or if it is not MJPEG.
bool readAvi(const std::string& path, std::vector<camera::FramePtr>& frames, int& fps);

}

#endif
