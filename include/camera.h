#ifndef CAMERA_H
#define CAMERA_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace camera {

enum class Zone { None, Field, Line, Goal };

struct Frame {
    int width = 0;
    int height = 0;
    long long timeMs = 0;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> jpeg;   // Set instead of rgb for goal_cam.py replay frames

    // Filled in by the detector.
    bool ball = false;
    int ballX = 0;
    int ballY = 0;
    int ballR = 0;
    Zone zone = Zone::None;
};

using FramePtr = std::shared_ptr<const Frame>;

struct Config {
    std::string device = "/dev/video0";
    std::string videoFile;   // Used instead of the camera when set (needs ffmpeg)
    bool simulate = false;
    int width = 320;
    int height = 240;
    int fps = 15;
};

// Callbacks run on the capture thread. Status is "live", "file", "sim" or "offline".
// A lost camera is reopened automatically.
void start(const Config& config,
           std::function<void(std::shared_ptr<Frame>)> onFrame,
           std::function<void(const std::string&)> onStatus);
void stop();

}

#endif
