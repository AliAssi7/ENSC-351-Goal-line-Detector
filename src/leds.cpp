#include "leds.h"
#include "gpio.h"
#include "pins.h"

#include <iostream>

namespace leds {

struct Led {
    const char* name;
    Color color;
    int fd;
    bool failed;
};

static Led ledList[2] = {
    {pins::kLedGreen, Color::Green, -1, false},
    {pins::kLedRed,   Color::Red,   -1, false}
};

bool init()
{
    bool ok = true;

    for (Led& led : ledList)
    {
        if (led.fd < 0)
            led.fd = gpio::openOutput(led.name, 0);

        if (led.fd < 0)
        {
            std::cerr << "LED " << led.name << " unavailable\n";
            ok = false;
        }
    }

    return ok;
}

bool show(Color color)
{
    bool ok = true;

    // Turn the other LED off before lighting the new one.
    for (int pass = 0; pass < 2; ++pass)
    {
        for (Led& led : ledList)
        {
            bool on = led.color == color;

            if (led.fd < 0 || on != (pass == 1))
                continue;

            if (!gpio::set(led.fd, on))
            {
                ok = false;
                if (!led.failed)
                    std::cerr << "LED " << led.name << ": GPIO write failed\n";
                led.failed = true;
            }
        }
    }

    return ok;
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
        led.failed = false;
    }
}

}
