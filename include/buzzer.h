#ifndef BUZZER_H
#define BUZZER_H

namespace buzzer {

bool init();

// Non-blocking; the buzzer turns itself off after the given time.
void beep(int ms);

void cleanup();

}

#endif
