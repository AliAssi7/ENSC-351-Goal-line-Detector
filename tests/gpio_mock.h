#ifndef GPIO_MOCK_H
#define GPIO_MOCK_H

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

void reset();

}

#endif
