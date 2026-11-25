#ifndef ENCODER_H
#define ENCODER_H

#include <cstdint>
#include <functional>

namespace encoder {

// Full-step quadrature decoder: contact bounce on one channel cancels
// itself out, so one detent produces exactly one step.
class Decoder {
public:
    // Returns 1 = clockwise, -1 = counterclockwise, 0 = no step.
    int update(int a, int b);

private:
    uint8_t state = 0;
};

// Reports a press once the (active-low) button has been stable for 20 ms.
class Button {
public:
    bool update(int level, long long nowMs);

private:
    int raw = 1;
    int stable = 1;
    long long changedAt = 0;
};

// Callbacks run on the encoder thread.
bool start(std::function<void(int)> onTurn, std::function<void()> onPress);
void stop();

}

#endif
