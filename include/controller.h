#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "camera.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Decision policy:
//  - IR: the existing beam check (beam broken, clear again 1 s later) opens
//    a possible-goal event.
//  - Manual: the referee requests a review from the dashboard.
//  - YOLO camera (goal_cam.py): keeps the replay and reports whether it saw
//    the ball around the trigger. This is supporting evidence only; it does
//    not show that the whole ball crossed the line.
//  - The test-only built-in camera (--sim/--video) can also open events.
// Another source within 5 s is added to the same event as corroboration.
// Nothing is awarded automatically: only the referee's decision sets GOAL.
//
// LEDs: green = operational or goal confirmed, red = fault (IR error or
// camera offline) or no goal, both off = review pending.
namespace controller {

enum class State { Monitoring, Review, Goal };
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
void setCaptureHook(std::function<void(const std::string& eventId)> hook);
void cameraEvidence(const std::string& eventId, const std::string& evidence, const std::string& replay);
void cameraClip(const std::string& eventId, std::vector<camera::FramePtr> frames, int fps);
void cameraFrame(const camera::FramePtr& frame);
void setCameraStatus(const std::string& status);
void setIrStatus(const std::string& status);

// A line from the server: REVIEW, DECISION <id> GOAL|NO_GOAL,
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
