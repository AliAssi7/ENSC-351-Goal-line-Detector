#ifndef GPIO_MOCK_H
#define GPIO_MOCK_H

#include <vector>

namespace gpio_mock {

void setInput(const char* name, int value);

// Last value written or set; -1 if the line was never opened.
int value(const char* name);
bool isOpen(const char* name);

// Number of low-to-high transitions written to an output.
int rises(const char* name);

// Square-wave measurements for a toggled output such as the passive buzzer.
int bursts(const char* name);
long long highTimeUs(const char* name);
long long lastBurstMs(const char* name);

// One continuous tone (a gap of more than 20 ms starts the next one).
struct Burst {
    int cycles = 0;
    long long startUs = 0;
    long long endUs = 0;   // last falling edge
    long long highUs = 0;
};
std::vector<Burst> burstLog(const char* name);

// A broken line cannot be opened, and reads/writes on it fail.
void setBroken(const char* name, bool broken);

void reset();

}

#endif
