#ifndef BUZZER_H
#define BUZZER_H

// Passive buzzer on GPIO25: 2000 Hz square wave at 25% duty.
namespace buzzer {

bool init();

// Non-blocking; sounds for the given time, then stops by itself.
// Ignored while a beep is already sounding.
void beep(int ms);

bool active();

void cleanup();

}

#endif
