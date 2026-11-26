#ifndef LEDS_H
#define LEDS_H

namespace leds {

enum class Color { Off, Green, Yellow, Red };

// Opens all LEDs (off). Returns false if any could not be opened;
// the others still work.
bool init();

// Lights only the given LED, so two are never on together.
// Returns false if a GPIO write failed.
bool show(Color color);

void cleanup();

}

#endif
