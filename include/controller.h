#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "camera.h"
#include "goalcam.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Hybrid decision policy (sensor fusion, not proof the whole ball crossed):
//  - IR (beam broken, clear again 1 s later) opens an event: Evaluating,
//    LEDs off, while goal_cam.py checks the camera footage around the break.
//  - Confident, event-correlated YOLO evidence: automatic Goal.
//  - Anything else (no ball, low confidence, too little footage, camera
//    unavailable, no answer within 8 s): Awaiting, yellow LED until the
//    referee decides. No ball is never an automatic No Goal.
//  - Manual reviews (Simulate Event) always wait for the referee.
//  - The camera alone never opens an event.
// Goal: green 3 s and the whistle. No Goal: red 3 s. Then Monitoring.
// The latest automatic goal keeps its replay and can be corrected once.
namespace controller {

enum class State { Monitoring, Evaluating, Awaiting, Goal, NoGoal };
enum class Source { Camera, Ir, Manual };

enum { LiveSlot = 0, ReplaySlot = 1 };

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
using SendText = std::function<void(const std::string&)>;
using SendFrame = std::function<void(int slot, const std::string& header, Bytes data)>;

// leds and buzzer must be initialized first.
void init(SendText sendText, SendFrame sendFrame, int liveFps);

// Resends the current state after the server (re)connects.
void resync();

void detected(Source source, long long triggerMs, long long nowMs);

// Called when an event opens, to start the camera's replay capture.
// breakOffsetMs: how long ago the beam broke. Returns false if the camera
// could not be asked.
using CaptureHook = std::function<bool(const std::string& eventId, int breakOffsetMs)>;
void setCaptureHook(CaptureHook hook);
void cameraEvidence(const std::string& eventId, const goalcam::Evidence& evidence,
                    const std::string& replay, long long nowMs);
void cameraClip(const std::string& eventId, std::vector<camera::FramePtr> frames, int fps);
void cameraFrame(const camera::FramePtr& frame);
void setCameraStatus(const std::string& status);
void setIrStatus(const std::string& status);

// A line from the server: REVIEW, DECISION <id> GOAL|NO_GOAL (also confirms
// or corrects the latest automatic goal), VIEW <id>|-,
// REPLAY NEXT|PREV|PLAY|PAUSE|TOGGLE or REPLAY SEEK <n>.
void message(const std::string& line, long long nowMs);

// Rotary encoder input.
void turn(int step);
void press();

void tick(long long nowMs);

State state();
std::string eventId();

}

#endif
