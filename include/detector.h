#ifndef DETECTOR_H
#define DETECTOR_H

#include "camera.h"

namespace detector {

// The goal line from (x1, y1) to (x2, y2), drawn on the goal-side edge of the
// painted line. (gx, gy) is any point inside the goal.
struct Line {
    int x1, y1, x2, y2, gx, gy;
};

Line defaultLine(int width, int height);
bool valid(const Line& line, int width, int height);

// HSV thresholds on a 0-359 hue and 0-255 saturation/value scale.
struct BallColor {
    int hue = 20;
    int hueTolerance = 15;
    int minSaturation = 110;
    int minValue = 70;
};

struct Result {
    bool found = false;
    int x = 0;
    int y = 0;
    int radius = 0;
    camera::Zone zone = camera::Zone::None;

    // The tracked ball has just moved from play to wholly over the line.
    bool crossing = false;
};

class Detector {
public:
    void setLine(const Line& line);
    const Line& line() const { return goalLine; }
    void setColor(const BallColor& color) { ballColor = color; }

    Result process(const camera::Frame& frame);

private:
    bool hasLine = false;
    Line goalLine{};
    BallColor ballColor;

    long long frameIndex = 0;
    long long lastInPlay = -1000;
    int goalFrames = 0;
    int missing = 0;
    bool tracking = false;
    int lastX = 0;
    int lastY = 0;
};

}

#endif
