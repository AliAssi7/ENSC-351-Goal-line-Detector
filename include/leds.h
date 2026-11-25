#ifndef LEDS_H
#define LEDS_H

namespace leds {

enum class Color { Off, Green, Yellow, Red };

bool init();

// Lights only the given LED.
void show(Color color);

void cleanup();

}

#endif
