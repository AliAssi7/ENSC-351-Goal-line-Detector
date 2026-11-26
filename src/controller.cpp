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

// How long the green (goal) or red (no goal) LED stays on after a decision.
static const long long kNotifyMs = 3000;
// An IR event with no final camera answer by then waits for the referee.
static const long long kEvidenceTimeoutMs = 8000;
// Another source within this time joins the same event.
static const long long kCorroborationMs = 5000;
static const long long kStatusIntervalMs = 1000;

static std::mutex mutex;
static State current = State::Monitoring;
static SendText sendText;
static SendFrame sendFrame;
static int liveIntervalMs = 125;
static long long lastLiveMs = -1000000;

static std::string session;
static int eventCount = 0;

// The event being evaluated or awaiting a decision.
static std::string currentId;
static std::vector<Source> sources;
static bool manualEvent = false;
static long long eventStartMs = 0;
static std::string awaitingReason;
static goalcam::Evidence evidence;
static std::string replayState;   // empty when no camera replay was requested

// The latest automatic goal, which the referee can replay and correct.
static std::string autoId;
static std::string autoSources;
static bool autoAcknowledged = false;
static bool autoCorrected = false;
static bool viewing = false;

static long long notifyUntil = 0;
static leds::Color ledColor = leds::Color::Off;
static long long ledOffAt = 0;

static std::string cameraStatus = "starting";
static std::string irStatus = "offline";
static std::string lastStatus;
static long long lastStatusMs = 0;
static std::string lastReplay;
static camera::FramePtr lastReplayFrame;
static CaptureHook captureHook;

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
    case State::Evaluating: return "evaluating";
    case State::Awaiting:   return "awaiting";
    case State::Goal:       return "goal";
    case State::NoGoal:     return "nogoal";
    default:                return "monitoring";
    }
}

static bool reviewing()
{
    return current == State::Evaluating || current == State::Awaiting;
}

static std::string sourceList()
{
    std::string list;

    for (Source source : sources)
        list += (list.empty() ? "" : ",") + std::string(sourceName(source));

    return list;
}

static void sendEvent()
{
    send("EVENT " + currentId + " " + sourceList());
}

static void sendCamera(const std::string& id)
{
    send("CAMERA " + id + " " + evidence.result + " " + replayState);

    if (evidence.measured)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "CONFIDENCE %s %d %d %d %d %d %d", id.c_str(),
                      evidence.runs, evidence.ballRuns, int(evidence.best * 100),
                      evidence.firstMs, evidence.lastMs, evidence.movePx);
        send(line);
    }
}

// Green or red lights for kNotifyMs; yellow stays on until a decision.
static void showLeds(leds::Color color, long long nowMs)
{
    leds::show(color);
    ledColor = color;
    ledOffAt = nowMs + kNotifyMs;
}

static void await(const std::string& reason, long long nowMs)
{
    current = State::Awaiting;
    awaitingReason = reason;
    showLeds(leds::Color::Yellow, nowMs);
    send("AWAITING " + currentId + " " + reason);
}

static void goal(long long nowMs)
{
    current = State::Goal;
    currentId.clear();
    notifyUntil = nowMs + kNotifyMs;
    showLeds(leds::Color::Green, nowMs);
    buzzer::whistle();
}

static void reject(long long nowMs)
{
    current = State::NoGoal;
    currentId.clear();
    notifyUntil = nowMs + kNotifyMs;
    showLeds(leds::Color::Red, nowMs);
}

// Only confident, event-correlated camera evidence on an IR event is a goal.
// No ball is never a "no goal": anything uncertain goes to the referee.
static void useEvidence(long long nowMs)
{
    const std::string& r = evidence.result;

    if (r == "confident")
    {
        autoId = currentId;
        autoSources = sourceList();
        autoAcknowledged = autoCorrected = false;
        send("AUTO " + autoId + " " + autoSources);
        goal(nowMs);   // the replay stays loaded for the referee
    }
    else if (r == "none" || r == "missed")
        await("no_ball", nowMs);
    else if (r == "low")
        await("low_confidence", nowMs);
    else if (r == "stationary")
        await("ball_not_moving", nowMs);
    else if (r == "insufficient")
        await("insufficient", nowMs);
    else if (r == "seen")
        await("unverified", nowMs);
    else if (r == "ignored")
        await("camera_busy", nowMs);
    else
        await("camera_unavailable", nowMs);
}

static void startEvent(Source source, long long triggerMs, long long nowMs)
{
    currentId = session + "-" + std::to_string(++eventCount);
    sources = {source};
    manualEvent = source == Source::Manual;
    eventStartMs = nowMs;
    viewing = false;

    replay::begin(currentId, triggerMs);
    lastReplay.clear();
    lastReplayFrame = nullptr;

    evidence = goalcam::Evidence();
    replayState.clear();
    current = State::Evaluating;
    sendEvent();

    bool cameraOk = false;

    if (captureHook)
    {
        replayState = "pending";
        cameraOk = captureHook(currentId, int(nowMs - triggerMs));

        if (!cameraOk)
            evidence.result = replayState = "unavailable";

        sendCamera(currentId);
        cameraOk = cameraOk && cameraStatus == "yolo";
    }

    if (manualEvent)
        await("manual", nowMs);
    else if (!cameraOk)
        await("camera_unavailable", nowMs);
    else
        showLeds(leds::Color::Off, nowMs);
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
    current = State::Monitoring;
    currentId.clear();
    autoId.clear();
    viewing = false;
    replay::clear();
    showLeds(leds::Color::Off, 0);
}

void resync()
{
    std::lock_guard<std::mutex> lock(mutex);

    lastStatus.clear();
    lastReplay.clear();
    lastReplayFrame = nullptr;
    lastLiveMs = -1000000;

    if (reviewing())
    {
        sendEvent();
        if (!replayState.empty())
            sendCamera(currentId);
        if (current == State::Awaiting)
            send("AWAITING " + currentId + " " + awaitingReason);
    }

    // An automatic goal sent while the server was away is sent again.
    if (!autoId.empty() && !autoAcknowledged)
        send("AUTO " + autoId + " " + autoSources);
}

void detected(Source source, long long triggerMs, long long nowMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    // The camera alone never opens an event; it can only support one.
    if (current == State::Monitoring && source != Source::Camera)
    {
        startEvent(source, triggerMs, nowMs);
    }
    else if (reviewing() && nowMs - eventStartMs <= kCorroborationMs &&
             std::find(sources.begin(), sources.end(), source) == sources.end())
    {
        sources.push_back(source);
        sendEvent();
    }
}

void setCaptureHook(CaptureHook hook)
{
    std::lock_guard<std::mutex> lock(mutex);
    captureHook = hook;
}

void cameraEvidence(const std::string& eventId, const goalcam::Evidence& update,
                    const std::string& replayUpdate, long long nowMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (!reviewing() || eventId != currentId || replayState.empty())
        return;

    evidence = update;
    if (replayState != "ready")
        replayState = replayUpdate;
    sendCamera(currentId);

    // Late evidence only updates the dashboard once the referee has the event.
    if (current == State::Evaluating && evidence.result != "pending")
        useEvidence(nowMs);
}

void cameraClip(const std::string& eventId, std::vector<camera::FramePtr> frames, int fps)
{
    std::lock_guard<std::mutex> lock(mutex);

    // An automatic goal's clip usually arrives after the decision.
    bool forCurrent = reviewing() && eventId == currentId && !replayState.empty();
    bool forAuto = !eventId.empty() && eventId == autoId && replay::status().eventId == autoId;

    if ((forCurrent || forAuto) && replay::load(eventId, std::move(frames), fps))
    {
        replayState = "ready";
        sendCamera(eventId);
    }
}

void cameraFrame(const camera::FramePtr& frame)
{
    std::lock_guard<std::mutex> lock(mutex);

    // The dashboard shows the replay instead of live video during a review.
    if (reviewing() || viewing || frame->timeMs - lastLiveMs < liveIntervalMs)
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

static void decision(const std::string& id, const std::string& result, long long nowMs)
{
    if (reviewing() && id == currentId)
    {
        if (result == "GOAL")
            goal(nowMs);
        else if (result == "NO_GOAL")
            reject(nowMs);
        return;
    }

    // The server confirms the latest automatic goal, or corrects it once.
    if (id.empty() || id != autoId)
        return;

    autoAcknowledged = true;

    if (result == "NO_GOAL" && !autoCorrected)
    {
        autoCorrected = true;
        viewing = false;
        if (!reviewing())
            reject(nowMs);
    }
}

void message(const std::string& line, long long nowMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    std::istringstream in(line);
    std::string command;
    in >> command;

    if (command == "REVIEW")
    {
        if (reviewing())
            sendEvent();
        else
            startEvent(Source::Manual, nowMs, nowMs);
    }
    else if (command == "DECISION")
    {
        std::string id, result;
        in >> id >> result;
        decision(id, result, nowMs);
    }
    else if (command == "VIEW")
    {
        // Replays the latest automatic goal while monitoring; "VIEW -" ends it.
        std::string id;
        in >> id;

        if (id == "-")
        {
            viewing = false;
            replay::setPlaying(false);
        }
        else if (!reviewing() && !id.empty() && id == autoId && replay::status().eventId == autoId)
        {
            viewing = true;
            lastReplay.clear();
            lastReplayFrame = nullptr;
        }
    }
    else if (command == "REPLAY" && (reviewing() || viewing))
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

    if (reviewing() || viewing)
        step > 0 ? replay::next() : replay::previous();
}

void press()
{
    std::lock_guard<std::mutex> lock(mutex);

    if (reviewing() || viewing)
        replay::togglePlayback();
}

void tick(long long nowMs)
{
    std::lock_guard<std::mutex> lock(mutex);

    replay::tick(nowMs);

    if (current == State::Evaluating && nowMs - eventStartMs >= kEvidenceTimeoutMs)
        await("timeout", nowMs);

    if ((current == State::Goal || current == State::NoGoal) && nowMs >= notifyUntil)
        current = State::Monitoring;

    if ((ledColor == leds::Color::Green || ledColor == leds::Color::Red) && nowMs >= ledOffAt)
        showLeds(leds::Color::Off, nowMs);

    std::string status = std::string("STATUS ") + stateName(current) + " " +
                         cameraStatus + " " + irStatus + " " +
                         (currentId.empty() ? "-" : currentId);

    if (status != lastStatus || nowMs - lastStatusMs >= kStatusIntervalMs)
    {
        send(status);
        lastStatus = status;
        lastStatusMs = nowMs;
    }

    if (!reviewing() && !viewing)
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
