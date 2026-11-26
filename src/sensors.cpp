#include "sensors.h"
#include "gpio.h"
#include "pins.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace sensors {

static const long long kConfirmMs = 1000;
static const long long kStuckMs = 5000;
static const int kPollMs = 10;

int BeamCheck::update(const int* levels, long long nowMs)
{
    for (int i = 0; i < kBeams; ++i)
    {
        if (ignored[i] && levels[i] == 1)
        {
            ignored[i] = false;
            std::cout << "IR " << pins::kIrBeams[i] << " clear again, back in use\n";
        }
    }

    if (step == Step::Armed)
    {
        for (int i = 0; i < kBeams; ++i)
        {
            if (levels[i] == 0 && !ignored[i])
            {
                beam = i;
                brokenMs = nowMs;
                step = Step::Waiting;
                std::cout << "IR " << pins::kIrBeams[i] << " beam broken, checking again in 1 s\n";
                break;
            }
        }
        return -1;
    }

    // The triggered beam is not sampled again until the 1 s check.
    if (step == Step::Waiting)
    {
        if (nowMs - brokenMs < kConfirmMs)
            return -1;

        if (levels[beam] == 1)
        {
            step = Step::Armed;
            std::cout << "IR " << pins::kIrBeams[beam] << ": possible GOAL\n";
            return beam;
        }

        if (levels[beam] == 0)
        {
            step = Step::Blocked;
            std::cout << "IR " << pins::kIrBeams[beam] << " still blocked after 1 s: no goal\n";
        }
        else
        {
            step = Step::Armed;
        }
        return -1;
    }

    // Blocked: wait for the beam to clear before re-arming.
    if (levels[beam] != 0)
    {
        step = Step::Armed;
        std::cout << "IR " << pins::kIrBeams[beam] << " clear, re-armed\n";
    }
    else if (nowMs - brokenMs > kStuckMs)
    {
        ignored[beam] = true;
        step = Step::Armed;
        std::cout << "IR " << pins::kIrBeams[beam] << " blocked for over 5 s: ignored until it "
                  << "clears (check the alignment)\n";
    }

    return -1;
}

static std::thread worker;
static std::atomic<bool> running{false};

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool start(const std::vector<std::string>& skip,
           std::function<void(long long, long long)> onGoal,
           std::function<void(const std::string&)> onStatus)
{
    if (running)
        return true;

    int fds[kBeams];
    int used = 0;
    int open = 0;

    for (int i = 0; i < kBeams; ++i)
    {
        fds[i] = -1;

        if (std::find(skip.begin(), skip.end(), pins::kIrBeams[i]) != skip.end())
        {
            std::cout << "IR " << pins::kIrBeams[i] << " skipped\n";
            continue;
        }

        ++used;
        fds[i] = gpio::openInput(pins::kIrBeams[i]);

        if (fds[i] < 0)
            std::cerr << "IR " << pins::kIrBeams[i] << " unavailable, continuing without it\n";
        else
            ++open;
    }

    if (used == 0)
    {
        onStatus("disabled");
        return false;
    }

    std::cout << "IR: " << open << " of " << used << " beams in use\n";
    running = true;

    worker = std::thread([fds, used, onGoal, onStatus]() mutable {
        BeamCheck check;
        std::string lastStatus;

        while (running)
        {
            int levels[kBeams];
            int working = 0;

            for (int i = 0; i < kBeams; ++i)
            {
                levels[i] = fds[i] >= 0 ? gpio::get(fds[i]) : -1;

                if (fds[i] >= 0 && levels[i] < 0)
                {
                    std::cerr << "IR " << pins::kIrBeams[i] << ": read failed, continuing without it\n";
                    gpio::close(fds[i]);
                    fds[i] = -1;
                }
            }

            long long now = nowMs();
            int beam = check.update(levels, now);

            if (beam >= 0)
                onGoal(check.brokenAt(), now);

            for (int i = 0; i < kBeams; ++i)
                working += fds[i] >= 0 && !check.stuck(i);

            std::string status = working == used ? "ok" : working > 0 ? "degraded" : "error";

            if (status != lastStatus)
            {
                if (!lastStatus.empty() || status != "ok")
                    std::cout << "IR status: " << status << " (" << working << " of " << used
                              << " beams)\n";
                onStatus(status);
                lastStatus = status;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }

        for (int fd : fds)
            gpio::close(fd);
    });

    return true;
}

void stop()
{
    if (!running.exchange(false))
        return;

    worker.join();
}

}
