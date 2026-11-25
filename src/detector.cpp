#include "detector.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace detector {

static const int kConfirmFrames = 2;   // Consecutive frames wholly over the line
static const int kArmFrames = 30;      // The ball must have been in play this recently
static const int kMaxMissing = 8;

using camera::Zone;

static double side(const Line& l, double px, double py)
{
    return double(l.x2 - l.x1) * (py - l.y1) - double(l.y2 - l.y1) * (px - l.x1);
}

Line defaultLine(int width, int height)
{
    int x = width * 6 / 10;
    return {x, 0, x, height - 1, width * 9 / 10, height / 2};
}

bool valid(const Line& l, int width, int height)
{
    auto inside = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < width && y < height;
    };

    double length = std::hypot(l.x2 - l.x1, l.y2 - l.y1);

    return inside(l.x1, l.y1) && inside(l.x2, l.y2) && inside(l.gx, l.gy) &&
           length >= 10 && std::fabs(side(l, l.gx, l.gy)) / length >= 2;
}

void Detector::setLine(const Line& line)
{
    goalLine = line;
    hasLine = true;
    tracking = false;
    goalFrames = 0;
    lastInPlay = -1000;
}

static bool isBall(const uint8_t* p, const BallColor& c)
{
    int r = p[0], g = p[1], b = p[2];
    int mx = std::max(r, std::max(g, b));
    int mn = std::min(r, std::min(g, b));
    int delta = mx - mn;

    if (mx < c.minValue || delta == 0 || delta * 255 < c.minSaturation * mx)
        return false;

    int hue;
    if (mx == r)
        hue = 60 * (g - b) / delta;
    else if (mx == g)
        hue = 120 + 60 * (b - r) / delta;
    else
        hue = 240 + 60 * (r - g) / delta;

    if (hue < 0)
        hue += 360;

    int diff = std::abs(hue - c.hue);
    return std::min(diff, 360 - diff) <= c.hueTolerance;
}

Result Detector::process(const camera::Frame& frame)
{
    const int w = frame.width;
    const int h = frame.height;
    Result result;

    ++frameIndex;

    if (!hasLine || !valid(goalLine, w, h))
        setLine(defaultLine(w, h));

    std::vector<uint8_t> mask(w * h);
    for (int i = 0; i < w * h; ++i)
        mask[i] = isBall(&frame.rgb[i * 3], ballColor);

    const Line& l = goalLine;
    double length = std::hypot(l.x2 - l.x1, l.y2 - l.y1);
    double sign = side(l, l.gx, l.gy) > 0 ? 1.0 : -1.0;

    // Largest connected blob of ball-coloured pixels, with the signed
    // distance (positive = goal side) of its nearest and furthest pixels.
    int bestArea = 0;
    double bestX = 0, bestY = 0, bestMin = 0, bestMax = 0;
    std::vector<int> stack;

    for (int start = 0; start < w * h; ++start)
    {
        if (mask[start] != 1)
            continue;

        int area = 0;
        double sumX = 0, sumY = 0;
        double minD = 1e9, maxD = -1e9;

        mask[start] = 2;
        stack.push_back(start);

        while (!stack.empty())
        {
            int i = stack.back();
            stack.pop_back();

            int x = i % w, y = i / w;
            double d = sign * side(l, x, y) / length;

            ++area;
            sumX += x;
            sumY += y;
            minD = std::min(minD, d);
            maxD = std::max(maxD, d);

            const int next[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1,
                                 y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};

            for (int n : next)
            {
                if (n >= 0 && mask[n] == 1)
                {
                    mask[n] = 2;
                    stack.push_back(n);
                }
            }
        }

        if (area > bestArea)
        {
            bestArea = area;
            bestX = sumX / area;
            bestY = sumY / area;
            bestMin = minD;
            bestMax = maxD;
        }
    }

    if (bestArea < std::max(12, w * h / 2000))
    {
        if (++missing > kMaxMissing)
        {
            tracking = false;
            goalFrames = 0;
        }
        return result;
    }

    result.found = true;
    result.x = int(bestX + 0.5);
    result.y = int(bestY + 0.5);
    result.radius = int(std::sqrt(bestArea / M_PI) + 0.5);

    if (bestMin > 0.5)
        result.zone = Zone::Goal;
    else if (bestMax >= 0)
        result.zone = Zone::Line;
    else
        result.zone = Zone::Field;

    // A large jump means a different object, which must be seen in play first.
    if (tracking && std::hypot(result.x - lastX, result.y - lastY) > std::max(w, h) / 3.0)
    {
        lastInPlay = -1000;
        goalFrames = 0;
    }

    tracking = true;
    missing = 0;
    lastX = result.x;
    lastY = result.y;

    if (result.zone != Zone::Goal)
    {
        goalFrames = 0;
        lastInPlay = frameIndex;
    }
    else if (++goalFrames == kConfirmFrames &&
             frameIndex - lastInPlay <= kArmFrames + kConfirmFrames)
    {
        result.crossing = true;
        lastInPlay = -1000;
    }

    return result;
}

}
