#ifndef PINS_H
#define PINS_H

// Header line names as reported by gpioinfo, or "gpiochipN:line" if the
// board's device tree does not name its lines.
namespace pins {

inline const char* kEncoderA = "GPIO16";       // Physical pin 36
inline const char* kEncoderB = "GPIO17";       // Physical pin 11
inline const char* kEncoderButton = "GPIO27";  // Physical pin 13
inline const char* kLedGreen = "GPIO5";        // Physical pin 29
inline const char* kLedYellow = "GPIO6";       // Physical pin 31
inline const char* kLedRed = "GPIO26";         // Physical pin 37
inline const char* kBuzzer = "GPIO25";         // Physical pin 22, through an NPN transistor

}

#endif
