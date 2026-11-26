#ifndef SENSORS_H
#define SENSORS_H

#include <functional>
#include <string>
#include <vector>

namespace sensors {

const int kBeams = 3;

// The three-beam check: a broken beam that is clear again 1 s later is a
// possible goal; a beam still blocked after 1 s is not, and the check waits
// for it to clear. A beam blocked for over 5 s is ignored until it clears,
// so one blocked or misaligned sensor cannot stop the others.
class BeamCheck {
public:
    // levels[i]: 1 = clear, 0 = blocked, -1 = unavailable.
    // Returns the beam that saw a goal, or -1.
    int update(const int* levels, long long nowMs);

    long long brokenAt() const { return brokenMs; }
    bool stuck(int beam) const { return ignored[beam]; }

private:
    enum class Step { Armed, Waiting, Blocked };
    Step step = Step::Armed;
    int beam = -1;
    long long brokenMs = 0;
    bool ignored[kBeams] = {};
};

// Status: "ok" (all used beams working), "degraded" (some), "error" (none).
// skip lists beams not to use, e.g. {"GPIO18"}. Callbacks run on the IR thread.
bool start(const std::vector<std::string>& skip,
           std::function<void(long long brokenMs, long long nowMs)> onGoal,
           std::function<void(const std::string&)> onStatus);
void stop();

}

#endif
