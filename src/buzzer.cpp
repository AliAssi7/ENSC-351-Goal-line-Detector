#include "buzzer.h"
#include "gpio.h"
#include "pins.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace buzzer {

using Clock = std::chrono::steady_clock;
using std::chrono::microseconds;
using std::chrono::milliseconds;

// Goal chime: tune these after listening to the buzzer.
// A note with frequency 0 is a silence.
struct Note {
    int frequencyHz;
    int ms;
};

static const Note kChime[] = {
    {1200, 150},
    {0,     80},
    {1600, 200},
};
static const int kDutyPercent = 15;   // Lower is softer

static int fd = -1;
static std::atomic<bool> running{false};
static bool sounding = false;
static bool writeFailed = false;
static std::mutex mutex;
static std::condition_variable wakeup;
static std::thread worker;

static void setPin(int value)
{
    if (!gpio::set(fd, value) && !writeFailed)
    {
        writeFailed = true;
        std::cerr << "Buzzer " << pins::kBuzzer << ": GPIO write failed\n";
    }
}

// GPIO25 has no hardware PWM. The short high phase is a busy-wait so a late
// wake-up cannot stretch it; the rest of each period sleeps.
static void tone(int frequencyHz, Clock::time_point end)
{
    const microseconds period(1000000 / frequencyHz);
    const microseconds high = period * kDutyPercent / 100;
    Clock::time_point next = Clock::now();

    while (running && next < end)
    {
        setPin(1);
        Clock::time_point fall = Clock::now() + high;
        while (Clock::now() < fall) {}
        setPin(0);

        next += period;
        std::this_thread::sleep_until(next < end ? next : end);

        if (Clock::now() - next > period)
            next = Clock::now();
    }

    setPin(0);
}

static void play()
{
    for (const Note& note : kChime)
    {
        Clock::time_point end = Clock::now() + milliseconds(note.ms);

        if (note.frequencyHz > 0)
        {
            tone(note.frequencyHz, end);
        }
        else
        {
            std::unique_lock<std::mutex> lock(mutex);
            wakeup.wait_until(lock, end, [] { return !running; });
        }

        if (!running)
            return;
    }
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

        lock.unlock();
        play();
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

    writeFailed = false;
    running = true;
    worker = std::thread(run);
    return true;
}

void chime()
{
    std::lock_guard<std::mutex> lock(mutex);

    // A chime already playing is not restarted, so repeated calls cannot overlap.
    if (!running || sounding)
        return;

    sounding = true;
    wakeup.notify_all();
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
        wakeup.notify_all();
    }

    worker.join();

    gpio::set(fd, 0);
    gpio::close(fd);
    fd = -1;
    sounding = false;
}

}
