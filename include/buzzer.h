#ifndef BUZZER_H
#define BUZZER_H

// Buzzer on GPIO25 (through an NPN transistor). Settings are at the top of buzzer.cpp.
namespace buzzer {

bool init();

// Non-blocking: plays the referee whistle (PIP - PIP - PIIIIIP) once.
// Ignored while it is already playing.
void whistle();

bool active();

// Stops any sound at once and leaves the pin low.
void cleanup();

}

#endif
