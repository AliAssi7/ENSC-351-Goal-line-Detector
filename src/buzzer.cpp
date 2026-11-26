#include "buzzer.h"
#include "gpio.h"
#include "pins.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace buzzer {

using Clock = std::chrono::steady_clock;

// GPIO25 has no hardware PWM, so the passive buzzer gets a software square wave.
static const int kFrequencyHz = 2000;
static const int kDutyPercent = 25;
static const auto kPeriod = std::chrono::microseconds(1000000 / kFrequencyHz);
static const auto kHigh = kPeriod * kDutyPercent / 100;

static int fd = -1;
static std::atomic<bool> running{false};
static bool sounding = false;
static Clock::time_point offAt;
static std::mutex mutex;
static std::condition_variable wakeup;
static std::thread worker;

// The short high phase is timed from the actual rising edge with a busy-wait,
// so late wake-ups cannot shrink it. Rising edges follow absolute deadlines,
// so they cannot lower the frequency either.
static void tone(Clock::time_point end)
{
    Clock::time_point next = Clock::now();

    while (running && next < end)
    {
        gpio::set(fd, 1);
        Clock::time_point fall = Clock::now() + kHigh;
        while (Clock::now() < fall) {}
        gpio::set(fd, 0);

        next += kPeriod;
        std::this_thread::sleep_until(next < end ? next : end);

        if (Clock::now() - next > kPeriod)
            next = Clock::now();
    }

    gpio::set(fd, 0);
}

static void run()
{
#ifdef __linux__
    prctl(PR_SET_TIMERSLACK, 1UL);
#endif

    std::unique_lock<std::mutex> lock(mutex);

    while (running)
    {
        wakeup.wait(lock, [] { return sounding || !running; });

        if (!running)
            break;

        Clock::time_point end = offAt;
        lock.unlock();
        tone(end);
        lock.lock();
        sounding = false;
    }
}

bool init()
{
    std::lock_guard<std::mutex> lock(mutex);

    if (running)
        return true;

    fd = gpio::openOutput(pins::kBuzzer, 0);

    if (fd < 0)
        return false;

    running = true;
    worker = std::thread(run);
    return true;
}

void beep(int ms)
{
    std::lock_guard<std::mutex> lock(mutex);

    // A beep already in progress is not extended, so repeated calls cannot chain.
    if (!running || sounding)
        return;

    offAt = Clock::now() + std::chrono::milliseconds(ms);
    sounding = true;
    wakeup.notify_one();
}

bool active()
{
    std::lock_guard<std::mutex> lock(mutex);
    return sounding;
}

void cleanup()
{
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (!running)
            return;

        running = false;
        wakeup.notify_one();
    }

    worker.join();

    gpio::set(fd, 0);
    gpio::close(fd);
    fd = -1;
    sounding = false;
}

}
