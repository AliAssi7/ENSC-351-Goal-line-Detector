#ifndef LEDS_H
#define LEDS_H

namespace leds {

enum class Color { Off, Green, Red };

bool init();

// Lights only the given LED.
void show(Color color);

void cleanup();

}

#endif
