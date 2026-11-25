#include "buzzer.h"
#include "gpio.h"
#include "pins.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace buzzer {

using Clock = std::chrono::steady_clock;

static int fd = -1;
static bool running = false;
static bool on = false;
static Clock::time_point offAt;
static std::mutex mutex;
static std::condition_variable wakeup;
static std::thread worker;

static void run()
{
    std::unique_lock<std::mutex> lock(mutex);

    while (running)
    {
        if (on && Clock::now() >= offAt)
        {
            gpio::set(fd, 0);
            on = false;
        }

        if (on)
            wakeup.wait_until(lock, offAt);
        else
            wakeup.wait(lock);
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

    if (!running)
        return;

    offAt = Clock::now() + std::chrono::milliseconds(ms);

    if (!on)
    {
        gpio::set(fd, 1);
        on = true;
    }

    wakeup.notify_one();
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
    on = false;
}

}
