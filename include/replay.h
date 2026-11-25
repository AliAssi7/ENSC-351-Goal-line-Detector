#ifndef REPLAY_H
#define REPLAY_H

#include "camera.h"

#include <string>
#include <vector>

namespace replay {

struct Status {
    std::string eventId;
    int frame = 0;
    int frames = 0;
    bool playing = false;
    bool complete = false;
};

void configure(int fps, int preMs, int postMs);

// Called for every captured frame.
void push(const camera::FramePtr& frame);

// Starts a new clip for an event: the buffered frames from preMs before
// triggerMs, followed by frames captured up to postMs after it.
void begin(const std::string& eventId, long long triggerMs);
void clear();

// Replaces the event's clip with finished frames (goal_cam.py replays).
bool load(const std::string& eventId, std::vector<camera::FramePtr> frames, int fps);

// Return true if the replay position or playback state changed.
bool next();
bool previous();
bool seek(int index);
bool setPlaying(bool playing);
bool togglePlayback();
bool tick(long long nowMs);

Status status();
camera::FramePtr current();

}

#endif
