#include "replay.h"

#include <deque>
#include <mutex>
#include <vector>

namespace replay {

using camera::FramePtr;

static std::mutex mutex;
static int frameMs = 66;
static long long preMs = 3000;
static long long postMs = 2000;

static std::deque<FramePtr> recent;
static std::vector<FramePtr> clip;
static std::string clipId;
static long long endMs = 0;
static bool active = false;
static bool complete = false;

static int clipFrameMs = 66;
static int position = 0;
static bool playing = false;
static long long lastAdvance = 0;

void configure(int fps, int pre, int post)
{
    std::lock_guard<std::mutex> lock(mutex);
    frameMs = 1000 / (fps > 0 ? fps : 1);
    preMs = pre;
    postMs = post;

    recent.clear();
    clip.clear();
    clipId.clear();
    active = complete = playing = false;
    position = 0;
}

void push(const FramePtr& frame)
{
    std::lock_guard<std::mutex> lock(mutex);

    // Extra second covers IR events, which are reported after a 1 s check.
    recent.push_back(frame);
    while (frame->timeMs - recent.front()->timeMs > preMs + 1000)
        recent.pop_front();

    if (active && !complete)
    {
        if (frame->timeMs <= endMs)
            clip.push_back(frame);
        else
            complete = true;
    }
}

void begin(const std::string& eventId, long long triggerMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    clip.clear();
    position = 0;

    for (const FramePtr& frame : recent)
    {
        if (frame->timeMs < triggerMs - preMs)
            continue;

        if (frame->timeMs <= triggerMs)
            position = int(clip.size());

        clip.push_back(frame);
    }

    clipId = eventId;
    clipFrameMs = frameMs;
    endMs = triggerMs + postMs;
    active = true;
    complete = false;
    playing = false;
}

void clear()
{
    std::lock_guard<std::mutex> lock(mutex);
    clip.clear();
    clipId.clear();
    active = false;
    complete = false;
    position = 0;
    playing = false;
}

bool load(const std::string& eventId, std::vector<FramePtr> frames, int fps)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (eventId != clipId)
        return false;

    clip = std::move(frames);
    clipFrameMs = 1000 / (fps > 0 ? fps : 15);
    active = true;
    complete = true;
    position = 0;
    playing = false;
    return true;
}

bool next()
{
    std::lock_guard<std::mutex> lock(mutex);

    if (position + 1 >= int(clip.size()))
        return false;

    ++position;
    return true;
}

bool previous()
{
    std::lock_guard<std::mutex> lock(mutex);

    if (position <= 0)
        return false;

    --position;
    return true;
}

bool seek(int index)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (index < 0 || index >= int(clip.size()) || index == position)
        return false;

    position = index;
    return true;
}

static bool setPlayingLocked(bool play)
{
    if (clip.empty() || playing == play)
        return false;

    if (play && complete && position == int(clip.size()) - 1)
        position = 0;

    playing = play;
    lastAdvance = 0;
    return true;
}

bool setPlaying(bool play)
{
    std::lock_guard<std::mutex> lock(mutex);
    return setPlayingLocked(play);
}

bool togglePlayback()
{
    std::lock_guard<std::mutex> lock(mutex);
    return setPlayingLocked(!playing);
}

bool tick(long long nowMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (active && !complete && nowMs > endMs + 500)
        complete = true;

    if (!playing || nowMs - lastAdvance < clipFrameMs)
        return false;

    lastAdvance = nowMs;

    if (position + 1 < int(clip.size()))
    {
        ++position;
        return true;
    }

    // Wait at the end of a clip that is still recording.
    if (complete)
    {
        playing = false;
        return true;
    }

    return false;
}

Status status()
{
    std::lock_guard<std::mutex> lock(mutex);
    return {clipId, position, int(clip.size()), playing, complete};
}

FramePtr current()
{
    std::lock_guard<std::mutex> lock(mutex);
    return clip.empty() ? nullptr : clip[position];
}

}
