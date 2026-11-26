#ifndef LEDS_H
#define LEDS_H

namespace leds {

enum class Color { Off, Green, Red };

// Opens both LEDs (off). Returns false if either could not be opened;
// the other one still works.
bool init();

// Lights only the given LED, so both are never on together.
// Returns false if a GPIO write failed.
bool show(Color color);

void cleanup();

}

#endif
