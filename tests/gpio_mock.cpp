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

struct Line {
    std::string name;
    int value;
    bool output;
    bool open;
    int rises;
};

std::mutex mutex;
std::vector<Line> lines;

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
    lines.push_back({name, value, output, true, 0});
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

    if (!line)
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

    if (!line || !line->output)
        return false;

    if (!line->value && value)
        ++line->rises;

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

void reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    lines.clear();
}

}
