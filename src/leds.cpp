#include "leds.h"
#include "gpio.h"
#include "pins.h"

namespace leds {

struct Led {
    const char* name;
    Color color;
    int fd;
};

static Led ledList[3] = {
    {pins::kLedGreen,  Color::Green,  -1},
    {pins::kLedYellow, Color::Yellow, -1},
    {pins::kLedRed,    Color::Red,    -1}
};

bool init()
{
    for (Led& led : ledList)
    {
        if (led.fd >= 0)
            continue;

        led.fd = gpio::openOutput(led.name, 0);

        if (led.fd < 0)
        {
            cleanup();
            return false;
        }
    }

    return true;
}

void show(Color color)
{
    for (Led& led : ledList)
    {
        if (led.fd >= 0)
            gpio::set(led.fd, led.color == color);
    }
}

void cleanup()
{
    for (Led& led : ledList)
    {
        if (led.fd < 0)
            continue;

        gpio::set(led.fd, 0);
        gpio::close(led.fd);
        led.fd = -1;
    }
}

}
