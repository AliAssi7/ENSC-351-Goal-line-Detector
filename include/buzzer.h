#ifndef BUZZER_H
#define BUZZER_H

// Passive buzzer on GPIO25, driven by a software square wave.
namespace buzzer {

bool init();

// Non-blocking: plays the short goal chime once (see buzzer.cpp for the
// notes). Ignored while the chime is already playing.
void chime();

bool active();

// Stops any sound at once and leaves the pin low.
void cleanup();

}

#endif
