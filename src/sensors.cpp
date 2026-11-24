
#include "sensors.h"

#include <iostream>
#include <cstdio>
#include <string>
#include <chrono>
#include <thread>

namespace sensors {

struct Beam {
    const char* chip;
    int line;
    const char* name;
};

// BeagleY-AI GPIO mappings (Debian)
static const Beam beams[3] = {
    {"gpiochip2", 11, "GPIO18"},  // Physical pin 12
    {"gpiochip0", 7,  "GPIO23"},  // Physical pin 16
    {"gpiochip0", 10, "GPIO24"}   // Physical pin 18
};

// Read a GPIO using gpioget.
// Returns: 1 = clear, 0 = blocked, -1 = error.
static int readGPIO(const std::string& chip, int line)
{
    std::string cmd =
        "gpioget -B pull-up " + chip + " " +
        std::to_string(line);

    FILE* pipe = popen(cmd.c_str(), "r");

    if (!pipe)
        return -1;

    char buffer[16];
    std::string result;

    if (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        result = buffer;

    pclose(pipe);

    if (result.find('0') != std::string::npos)
        return 0;

    if (result.find('1') != std::string::npos)
        return 1;

    return -1;
}

// Sensor numbers: 1, 2, or 3.
int readBeam(int sensorNumber)
{
    if (sensorNumber < 1 || sensorNumber > 3)
        return -1;

    const Beam& beam = beams[sensorNumber - 1];

    return readGPIO(beam.chip, beam.line);
}

// Monotonic timestamp in milliseconds.
static long long getTimestampMs()
{
    auto now = std::chrono::steady_clock::now();

    return std::chrono::duration_cast<
        std::chrono::milliseconds
    >(now.time_since_epoch()).count();
}

// Returns:
// 1  = GOAL
// -1 = GPIO error
int runGoalDetection()
{
    std::cout << "Goal-line system armed\n";
    std::cout << "1 = clear, 0 = blocked\n";
    std::cout << "Waiting for beam break...\n\n";

    while (true)
    {
        int triggered = -1;

        // FIRST CHECK: Monitor all three beams.
        for (int i = 0; i < 3; ++i)
        {
            int state = readBeam(i + 1);

            if (state == -1)
            {
                std::cerr << "GPIO read error\n";
                return -1;
            }

            if (state == 0)
            {
                triggered = i;
                break;
            }
        }

        if (triggered != -1)
        {
            const Beam& beam = beams[triggered];

            std::cout << beam.name
                      << " beam broken.\n";
            std::cout << "Waiting 1 second...\n";

            // Wait without sampling the triggered sensor.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000)
            );

            // SECOND CHECK: Same beam, one reading.
            int secondCheck = readBeam(triggered + 1);

            if (secondCheck == 1)
            {
                std::cout << "\nGOAL\n";
                std::cout << "Sensor: "
                          << beam.name << "\n";
                std::cout << "Timestamp: "
                          << getTimestampMs()
                          << " ms\n";

                std::cout << "Goal latched.\n";

                return 1;
            }
            else if (secondCheck == 0)
            {
                std::cout << "NO GOAL\n";
                std::cout << beam.name
                          << " still blocked after 1 second.\n";

                // Wait until the obstruction clears.
                while (true)
                {
                    int state = readBeam(triggered + 1);

                    if (state == -1)
                    {
                        std::cerr << "GPIO read error\n";
                        return -1;
                    }

                    if (state == 1)
                        break;

                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(50)
                    );
                }

                std::cout << "Beam clear. Re-armed.\n\n";
            }
            else
            {
                std::cerr << "GPIO read error\n";
                return -1;
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(10)
        );
    }
}

} // namespace sensors
