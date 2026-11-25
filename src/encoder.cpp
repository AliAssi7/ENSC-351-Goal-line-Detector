#include "encoder.h"
#include "gpio.h"
#include "pins.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace encoder {

static const long long kDebounceMs = 20;

enum : uint8_t {
    START, CW_FINAL, CW_BEGIN, CW_NEXT, CCW_BEGIN, CCW_FINAL, CCW_NEXT,
    DIR_CW = 0x10, DIR_CCW = 0x20
};

// Indexed by [state][(a << 1) | b]; a step is only emitted after a full
// valid sequence returns to the rest position (both high).
static const uint8_t kTable[7][4] = {
    {START,     CW_BEGIN,  CCW_BEGIN, START},
    {CW_NEXT,   START,     CW_FINAL,  START | DIR_CW},
    {CW_NEXT,   CW_BEGIN,  START,     START},
    {CW_NEXT,   CW_BEGIN,  CW_FINAL,  START},
    {CCW_NEXT,  START,     CCW_BEGIN, START},
    {CCW_NEXT,  CCW_FINAL, START,     START | DIR_CCW},
    {CCW_NEXT,  CCW_FINAL, CCW_BEGIN, START},
};

int Decoder::update(int a, int b)
{
    state = kTable[state & 0x0f][(a ? 2 : 0) | (b ? 1 : 0)];

    if (state & DIR_CW)
        return 1;

    if (state & DIR_CCW)
        return -1;

    return 0;
}

bool Button::update(int level, long long nowMs)
{
    if (level != raw)
    {
        raw = level;
        changedAt = nowMs;
        return false;
    }

    if (raw != stable && nowMs - changedAt >= kDebounceMs)
    {
        stable = raw;
        return stable == 0;
    }

    return false;
}

static std::thread worker;
static std::atomic<bool> running{false};
static int fds[3] = {-1, -1, -1};

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void closeLines()
{
    for (int& fd : fds)
    {
        gpio::close(fd);
        fd = -1;
    }
}

bool start(std::function<void(int)> onTurn, std::function<void()> onPress)
{
    if (running)
        return true;

    fds[0] = gpio::openInput(pins::kEncoderA);
    fds[1] = gpio::openInput(pins::kEncoderB);
    fds[2] = gpio::openInput(pins::kEncoderButton);

    if (fds[0] < 0 || fds[1] < 0 || fds[2] < 0)
    {
        closeLines();
        return false;
    }

    running = true;

    worker = std::thread([onTurn, onPress] {
        Decoder decoder;
        Button button;

        while (running)
        {
            gpio::wait(fds, 3, 5);

            int a = gpio::get(fds[0]);
            int b = gpio::get(fds[1]);
            int sw = gpio::get(fds[2]);

            if (a >= 0 && b >= 0)
            {
                int step = decoder.update(a, b);
                if (step != 0)
                    onTurn(step);
            }

            if (sw >= 0 && button.update(sw, nowMs()))
                onPress();
        }
    });

    return true;
}

void stop()
{
    if (!running.exchange(false))
        return;

    worker.join();
    closeLines();
}

}
