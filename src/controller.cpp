#include "controller.h"
#include "buzzer.h"
#include "leds.h"
#include "replay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <sstream>

namespace controller {

static const int kBeepMs = 1000;
static const long long kGoalHoldMs = 5000;
static const long long kEvidenceWindowMs = 5000;
static const long long kStatusIntervalMs = 1000;

static std::mutex mutex;
static State current = State::Monitoring;
static SendText sendText;
static SendFrame sendFrame;
static int liveIntervalMs = 125;
static long long lastLiveMs = -1000000;

static std::string session;
static int eventCount = 0;
static std::string currentId;
static std::vector<Source> sources;
static long long eventStartMs = 0;
static long long goalSince = -1;
static long long rejectedUntil = 0;
static leds::Color shownLeds = leds::Color::Off;
static bool ledsShown = false;

static std::string cameraStatus = "starting";
static std::string irStatus = "offline";
static std::string lastStatus;
static long long lastStatusMs = 0;
static std::string lastReplay;
static camera::FramePtr lastReplayFrame;
static std::function<void(const std::string&)> captureHook;
static std::string cameraEvidenceState;
static std::string cameraReplayState;

static void sendCamera();

static void send(const std::string& line)
{
    if (sendText)
        sendText(line);
}

static void sendFrameData(int slot, const char* kind, const std::string& id,
                          int index, const camera::FramePtr& f)
{
    if (!sendFrame)
        return;

    bool jpeg = !f->jpeg.empty();
    const std::vector<uint8_t>& data = jpeg ? f->jpeg : f->rgb;

    std::ostringstream header;
    header << "FRAME " << kind << " " << id << " " << index << " "
           << f->width << " " << f->height << " "
           << (f->ball ? f->ballX : -1) << " " << (f->ball ? f->ballY : -1) << " "
           << (f->ball ? f->ballR : 0) << " " << int(f->zone) << " " << data.size()
           << (jpeg ? " jpeg" : " rgb");

    sendFrame(slot, header.str(), Bytes(f, &data));
}

static const char* sourceName(Source source)
{
    switch (source)
    {
    case Source::Camera: return "camera";
    case Source::Ir:     return "ir";
    default:             return "manual";
    }
}

static const char* stateName(State state)
{
    switch (state)
    {
    case State::Review: return "review";
    case State::Goal:   return "goal";
    default:            return "monitoring";
    }
}

static void sendEvent()
{
    std::string list;

    for (Source source : sources)
        list += (list.empty() ? "" : ",") + std::string(sourceName(source));

    send("EVENT " + currentId + " " + list);
}

static void updateLeds(long long nowMs)
{
    leds::Color color = leds::Color::Green;

    if (current == State::Review)
        color = leds::Color::Off;
    else if (current == State::Monitoring &&
             (nowMs < rejectedUntil || irStatus == "error" || cameraStatus == "offline"))
        color = leds::Color::Red;

    if (!ledsShown || color != shownLeds)
    {
        leds::show(color);
        shownLeds = color;
        ledsShown = true;
    }
}

static void startEvent(Source source, long long triggerMs, long long nowMs)
{
    current = State::Review;
    currentId = session + "-" + std::to_string(++eventCount);
    sources = {source};
    eventStartMs = nowMs;

    replay::begin(currentId, triggerMs);
    lastReplay.clear();
    lastReplayFrame = nullptr;

    updateLeds(nowMs);
    sendEvent();

    cameraEvidenceState.clear();
    cameraReplayState.clear();

    if (captureHook)
    {
        cameraEvidenceState = cameraReplayState = "pending";
        sendCamera();
        captureHook(currentId);
    }
}

static void monitor(long long nowMs, bool rejected)
{
    current = State::Monitoring;
    currentId.clear();
    replay::clear();
    rejectedUntil = rejected ? nowMs + kGoalHoldMs : 0;
    updateLeds(nowMs);
}

static void goal(long long nowMs)
{
    current = State::Goal;
    currentId.clear();
    replay::clear();
    updateLeds(nowMs);
    buzzer::beep(kBeepMs);
    goalSince = -1;
}

void init(SendText text, SendFrame frame, int liveFps)
{
    std::lock_guard<std::mutex> lock(mutex);

    sendText = text;
    sendFrame = frame;
    liveIntervalMs = 1000 / std::max(1, liveFps);
    // The board has no battery clock, so the random part keeps ids unique across reboots.
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "%04x", unsigned(std::random_device()() & 0xffff));
    session = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) + suffix;

    lastStatus.clear();
    ledsShown = false;
    monitor(0, false);
}

void resync()
{
    std::lock_guard<std::mutex> lock(mutex);

    lastStatus.clear();
    lastReplay.clear();
    lastReplayFrame = nullptr;
    lastLiveMs = -1000000;

    if (current == State::Review)
    {
        sendEvent();
        if (!cameraEvidenceState.empty())
            sendCamera();
    }
}

void detected(Source source, long long triggerMs, long long nowMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (current == State::Monitoring)
    {
        startEvent(source, triggerMs, nowMs);
    }
    else if (current == State::Review &&
             nowMs - eventStartMs <= kEvidenceWindowMs &&
             std::find(sources.begin(), sources.end(), source) == sources.end())
    {
        sources.push_back(source);
        sendEvent();
    }
}

void setCaptureHook(std::function<void(const std::string&)> hook)
{
    std::lock_guard<std::mutex> lock(mutex);
    captureHook = hook;
}

static void sendCamera()
{
    send("CAMERA " + currentId + " " + cameraEvidenceState + " " + cameraReplayState);
}

void cameraEvidence(const std::string& eventId, const std::string& evidence, const std::string& replayState)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (current != State::Review || eventId != currentId || cameraEvidenceState.empty())
        return;

    cameraEvidenceState = evidence;
    if (cameraReplayState != "ready")
        cameraReplayState = replayState;
    sendCamera();
}

void cameraClip(const std::string& eventId, std::vector<camera::FramePtr> frames, int fps)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (current != State::Review || eventId != currentId || cameraEvidenceState.empty())
        return;

    if (replay::load(eventId, std::move(frames), fps))
    {
        cameraReplayState = "ready";
        sendCamera();
    }
}

void cameraFrame(const camera::FramePtr& frame)
{
    std::lock_guard<std::mutex> lock(mutex);

    // The dashboard shows the replay instead of live video during a review.
    if (current == State::Review || frame->timeMs - lastLiveMs < liveIntervalMs)
        return;

    lastLiveMs = frame->timeMs;
    sendFrameData(LiveSlot, "LIVE", "-", 0, frame);
}

void setCameraStatus(const std::string& status)
{
    std::lock_guard<std::mutex> lock(mutex);
    cameraStatus = status;
}

void setIrStatus(const std::string& status)
{
    std::lock_guard<std::mutex> lock(mutex);
    irStatus = status;
}

void message(const std::string& line, long long nowMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    std::istringstream in(line);
    std::string command;
    in >> command;

    if (command == "REVIEW")
    {
        if (current == State::Review)
            sendEvent();
        else
            startEvent(Source::Manual, nowMs, nowMs);
    }
    else if (command == "DECISION")
    {
        std::string id, result;
        in >> id >> result;

        if (current != State::Review || id != currentId)
            return;

        if (result == "GOAL")
            goal(nowMs);
        else if (result == "NO_GOAL")
            monitor(nowMs, true);
    }
    else if (command == "REPLAY" && current == State::Review)
    {
        std::string action;
        int index = 0;
        in >> action;

        if (action == "NEXT")
            replay::next();
        else if (action == "PREV")
            replay::previous();
        else if (action == "PLAY")
            replay::setPlaying(true);
        else if (action == "PAUSE")
            replay::setPlaying(false);
        else if (action == "TOGGLE")
            replay::togglePlayback();
        else if (action == "SEEK" && in >> index)
            replay::seek(index);
    }
}

void turn(int step)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (current == State::Review)
        step > 0 ? replay::next() : replay::previous();
}

void press()
{
    std::lock_guard<std::mutex> lock(mutex);

    if (current == State::Review)
        replay::togglePlayback();
}

void tick(long long nowMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    replay::tick(nowMs);

    if (current == State::Goal)
    {
        if (goalSince < 0)
            goalSince = nowMs;
        else if (nowMs - goalSince >= kGoalHoldMs)
            monitor(nowMs, false);
    }

    updateLeds(nowMs);

    std::string status = std::string("STATUS ") + stateName(current) + " " +
                         cameraStatus + " " + irStatus + " " +
                         (currentId.empty() ? "-" : currentId);

    if (status != lastStatus || nowMs - lastStatusMs >= kStatusIntervalMs)
    {
        send(status);
        lastStatus = status;
        lastStatusMs = nowMs;
    }

    if (current != State::Review)
        return;

    replay::Status rs = replay::status();
    std::string line = "REPLAY " + rs.eventId + " " + std::to_string(rs.frame) + " " +
                       std::to_string(rs.frames) + " " + (rs.playing ? "1" : "0") +
                       " " + (rs.complete ? "1" : "0");

    if (line != lastReplay)
    {
        send(line);
        lastReplay = line;
    }

    camera::FramePtr frame = replay::current();

    if (frame && frame != lastReplayFrame)
    {
        sendFrameData(ReplaySlot, "REPLAY", rs.eventId, rs.frame, frame);
        lastReplayFrame = frame;
    }
}

State state()
{
    std::lock_guard<std::mutex> lock(mutex);
    return current;
}

std::string eventId()
{
    std::lock_guard<std::mutex> lock(mutex);
    return currentId;
}

}
