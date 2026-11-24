
#ifndef SENSORS_H
#define SENSORS_H

namespace sensors {

// Read a sensor:
// 1 = clear, 0 = blocked, -1 = error
int readBeam(int sensorNumber);

// Monitor all three beams until GOAL or an error.
// Returns 1 for GOAL, -1 for error.
int runGoalDetection();

}

#endif
