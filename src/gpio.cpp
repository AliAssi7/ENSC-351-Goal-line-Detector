#include "gpio.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace gpio {

static bool findLine(const char* name, std::string& chip, unsigned& offset)
{
    const char* colon = std::strchr(name, ':');

    if (colon)
    {
        chip = "/dev/" + std::string(name, colon - name);
        offset = std::strtoul(colon + 1, nullptr, 10);
        return true;
    }

    for (int n = 0; n < 16; ++n)
    {
        std::string path = "/dev/gpiochip" + std::to_string(n);
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);

        if (fd < 0)
            continue;

        gpiochip_info info{};

        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0)
        {
            for (unsigned i = 0; i < info.lines; ++i)
            {
                gpio_v2_line_info line{};
                line.offset = i;

                if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &line) == 0 &&
                    std::strcmp(line.name, name) == 0)
                {
                    ::close(fd);
                    chip = path;
                    offset = i;
                    return true;
                }
            }
        }

        ::close(fd);
    }

    return false;
}

static int request(const char* name, unsigned long long flags, int value)
{
    std::string chip;
    unsigned offset = 0;

    if (!findLine(name, chip, offset))
    {
        std::cerr << "GPIO line " << name << " not found\n";
        return -1;
    }

    int chipFd = open(chip.c_str(), O_RDONLY | O_CLOEXEC);

    if (chipFd < 0)
    {
        std::cerr << chip << ": " << std::strerror(errno) << "\n";
        return -1;
    }

    gpio_v2_line_request req{};
    req.offsets[0] = offset;
    req.num_lines = 1;
    std::strncpy(req.consumer, "true-goal", sizeof(req.consumer) - 1);
    req.config.flags = flags;

    if (flags & GPIO_V2_LINE_FLAG_OUTPUT)
    {
        req.config.num_attrs = 1;
        req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = value ? 1 : 0;
        req.config.attrs[0].mask = 1;
    }

    int rc = ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req);
    int err = errno;
    ::close(chipFd);

    if (rc < 0)
    {
        std::cerr << "Cannot request " << name << " (" << chip << " line "
                  << offset << "): " << std::strerror(err) << "\n";
        return -1;
    }

    return req.fd;
}

int openInput(const char* name)
{
    unsigned long long flags =
        GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;

    int fd = request(name, flags | GPIO_V2_LINE_FLAG_EDGE_RISING |
                               GPIO_V2_LINE_FLAG_EDGE_FALLING, 0);

    // Lines without interrupt support still work by polling.
    if (fd < 0)
        fd = request(name, flags, 0);

    return fd;
}

int openOutput(const char* name, int value)
{
    return request(name, GPIO_V2_LINE_FLAG_OUTPUT, value);
}

int get(int fd)
{
    gpio_v2_line_values values{};
    values.mask = 1;

    if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
        return -1;

    return values.bits & 1;
}

bool set(int fd, int value)
{
    gpio_v2_line_values values{};
    values.mask = 1;
    values.bits = value ? 1 : 0;

    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values) == 0;
}

bool wait(const int* fds, int count, int timeoutMs)
{
    pollfd polls[8];

    if (count > 8)
        count = 8;

    for (int i = 0; i < count; ++i)
        polls[i] = {fds[i], POLLIN, 0};

    if (poll(polls, count, timeoutMs) <= 0)
        return false;

    for (int i = 0; i < count; ++i)
    {
        if (polls[i].revents & POLLIN)
        {
            gpio_v2_line_event events[16];
            if (read(fds[i], events, sizeof(events)) < 0)
                return false;
        }
    }

    return true;
}

void close(int fd)
{
    if (fd >= 0)
        ::close(fd);
}

}
