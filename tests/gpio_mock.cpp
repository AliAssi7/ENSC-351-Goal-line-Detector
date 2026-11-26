#include "gpio.h"
#include "gpio_mock.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Line {
    std::string name;
    int value;
    bool output;
    bool open;
    int rises;

    // Square-wave bookkeeping: a gap of more than 20 ms starts a new burst.
    int bursts = 0;
    long long highUs = 0;
    Clock::time_point lastRise{};
    Clock::time_point lastFall{};
    Clock::time_point burstStart{};
    std::vector<gpio_mock::Burst> log;
};

std::mutex mutex;
std::vector<Line> lines;
std::vector<std::string> broken;
const Clock::time_point epoch = Clock::now();

bool isBroken(const std::string& name)
{
    for (const std::string& b : broken)
        if (b == name)
            return true;
    return false;
}

long long sinceEpochUs(Clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(t - epoch).count();
}

Line* find(const char* name)
{
    for (auto it = lines.rbegin(); it != lines.rend(); ++it)
        if (it->name == name)
            return &*it;
    return nullptr;
}

Line* byFd(int fd)
{
    size_t i = fd - 3;
    return fd >= 3 && i < lines.size() && lines[i].open ? &lines[i] : nullptr;
}

int add(const char* name, int value, bool output)
{
    if (isBroken(name))
        return -1;

    lines.push_back(Line{name, value, output, true, 0, 0, 0, {}, {}, {}, {}});
    return static_cast<int>(lines.size()) + 2;
}

}

namespace gpio {

int openInput(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex);
    return add(name, 1, false);
}

int openOutput(const char* name, int value)
{
    std::lock_guard<std::mutex> lock(mutex);
    return add(name, value, true);
}

int get(int fd)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = byFd(fd);

    if (!line || isBroken(line->name))
        return -1;

    // Lets a script drive inputs by writing 0/1 to <dir>/<line name>.
    const char* dir = std::getenv("TRUE_GOAL_MOCK_INPUTS");

    if (dir && !line->output)
    {
        std::ifstream file(std::string(dir) + "/" + line->name);
        char c;
        if (file >> c && (c == '0' || c == '1'))
            line->value = c - '0';
    }

    return line->value;
}

bool set(int fd, int value)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = byFd(fd);

    if (!line || !line->output || isBroken(line->name))
        return false;

    Clock::time_point now = Clock::now();

    if (!line->value && value)
    {
        ++line->rises;
        if (line->rises == 1 || now - line->lastFall > std::chrono::milliseconds(20))
        {
            ++line->bursts;
            line->burstStart = now;
            line->log.push_back({});
            line->log.back().startUs = sinceEpochUs(now);
        }
        line->lastRise = now;
        ++line->log.back().cycles;
    }
    else if (line->value && !value)
    {
        long long high = std::chrono::duration_cast<std::chrono::microseconds>(now - line->lastRise).count();
        line->highUs += high;
        line->lastFall = now;
        if (!line->log.empty())
        {
            line->log.back().highUs += high;
            line->log.back().endUs = sinceEpochUs(now);
        }
    }

    if (line->value != value && std::getenv("TRUE_GOAL_GPIO_LOG"))
        std::cerr << "[gpio " << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count()
                  << "] " << line->name << " = " << value << "\n";

    line->value = value;
    return true;
}

bool wait(const int*, int, int timeoutMs)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    return false;
}

void close(int fd)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = byFd(fd);
    if (line)
        line->open = false;
}

}

namespace gpio_mock {

void setInput(const char* name, int value)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = find(name);
    if (line)
        line->value = value;
}

int value(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = find(name);
    return line ? line->value : -1;
}

bool isOpen(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = find(name);
    return line && line->open;
}

int rises(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = find(name);
    return line ? line->rises : 0;
}

int bursts(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = find(name);
    return line ? line->bursts : 0;
}

long long highTimeUs(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = find(name);
    return line ? line->highUs : 0;
}

long long lastBurstMs(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = find(name);
    if (!line || line->bursts == 0)
        return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(line->lastFall - line->burstStart).count();
}

std::vector<Burst> burstLog(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex);
    Line* line = find(name);
    return line ? line->log : std::vector<Burst>();
}

void setBroken(const char* name, bool on)
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> kept;
    for (const std::string& b : broken)
        if (b != name)
            kept.push_back(b);
    if (on)
        kept.push_back(name);
    broken = kept;
}

void reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    lines.clear();
    broken.clear();
}

}
