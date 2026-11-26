#include "buzzer.h"
#include "camera.h"
#include "controller.h"
#include "detector.h"
#include "encoder.h"
#include "goalcam.h"
#include "gpio_mock.h"
#include "leds.h"
#include "network.h"
#include "pins.h"
#include "replay.h"
#include "sensors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << __FILE__ << ":" << __LINE__ << ": " #cond "\n";   \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

using camera::Zone;

static void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static bool startsWith(const std::string& s, const std::string& prefix)
{
    return s.rfind(prefix, 0) == 0;
}

// --- Synthetic frames -------------------------------------------------------

static const int W = 320, H = 240;

static std::shared_ptr<camera::Frame> makeFrame(long long timeMs, double bx = -1,
                                                double by = H / 2, double r = 12,
                                                int lineX = W * 6 / 10)
{
    auto f = std::make_shared<camera::Frame>();
    f->width = W;
    f->height = H;
    f->timeMs = timeMs;
    f->rgb.resize(W * H * 3);

    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            uint8_t* p = &f->rgb[(y * W + x) * 3];

            if (bx >= 0 && (x - bx) * (x - bx) + (y - by) * (y - by) <= r * r)
                p[0] = 235, p[1] = 110, p[2] = 25;
            else if (lineX > 0 && x >= lineX - 3 && x < lineX)
                p[0] = p[1] = p[2] = 235;
            else
                p[0] = 40, p[1] = 130, p[2] = 50;
        }
    }

    return f;
}

// Moves the ball from x0 to x1 in the given number of frames; returns crossings.
static int roll(detector::Detector& d, double x0, double x1, int frames,
                double y = H / 2, Zone* lastZone = nullptr)
{
    int crossings = 0;

    for (int i = 0; i <= frames; ++i)
    {
        double x = x0 + (x1 - x0) * i / frames;
        detector::Result r = d.process(*makeFrame(0, x, y));
        crossings += r.crossing;
        if (lastZone)
            *lastZone = r.zone;
    }

    return crossings;
}

static int hold(detector::Detector& d, double x, int frames, double y = H / 2)
{
    int crossings = 0;
    for (int i = 0; i < frames; ++i)
        crossings += d.process(*makeFrame(0, x, y)).crossing;
    return crossings;
}

static int empty(detector::Detector& d, int frames)
{
    int crossings = 0;
    for (int i = 0; i < frames; ++i)
        crossings += d.process(*makeFrame(0)).crossing;
    return crossings;
}

// --- Encoder ----------------------------------------------------------------

static int feed(encoder::Decoder& d, const std::vector<std::pair<int, int>>& seq)
{
    int total = 0;
    for (auto& ab : seq)
        total += d.update(ab.first, ab.second);
    return total;
}

static void testDecoder()
{
    encoder::Decoder d;
    std::vector<std::pair<int, int>> cw = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
    std::vector<std::pair<int, int>> ccw = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};

    CHECK(feed(d, cw) == 1);
    CHECK(feed(d, ccw) == -1);

    int sum = 0;
    for (int i = 0; i < 10; ++i)
        sum += feed(d, cw);
    CHECK(sum == 10);

    CHECK(feed(d, {{0, 1}, {1, 1}, {0, 1}, {1, 1}, {0, 1}, {0, 0},
                   {1, 0}, {0, 0}, {1, 0}, {1, 1}, {1, 0}, {1, 1}}) == 1);
    CHECK(feed(d, {{0, 1}, {0, 0}, {0, 1}, {1, 1}}) == 0);

    CHECK(d.update(0, 1) == 0);
    CHECK(d.update(0, 0) == 0);
    CHECK(d.update(1, 0) == 0);
    CHECK(d.update(1, 1) == 1);
    CHECK(d.update(1, 1) == 0);
}

static void testButton()
{
    encoder::Button b;
    int presses = 0;
    long long t = 1000;

    int levels[] = {0, 1, 0, 1, 0};
    for (int level : levels)
        presses += b.update(level, t++);
    for (int i = 0; i < 500; ++i)
        presses += b.update(0, t++);
    for (int level : levels)
        presses += b.update(1 - level, t++);
    for (int i = 0; i < 50; ++i)
        presses += b.update(1, t++);
    CHECK(presses == 1);

    for (int i = 0; i < 5; ++i)
        presses += b.update(0, t++);
    for (int i = 0; i < 50; ++i)
        presses += b.update(1, t++);
    CHECK(presses == 1);

    for (int i = 0; i < 25; ++i)
        presses += b.update(0, t++);
    CHECK(presses == 2);
}

static void testEncoderThread()
{
    gpio_mock::reset();
    std::atomic<int> turns{0};
    std::atomic<int> presses{0};

    CHECK(encoder::start([&](int step) { turns += step; }, [&] { ++presses; }));

    auto step = [](int a, int b) {
        gpio_mock::setInput("GPIO16", a);
        gpio_mock::setInput("GPIO17", b);
        sleepMs(15);
    };

    step(0, 1); step(0, 0); step(1, 0); step(1, 1);
    step(0, 1); step(0, 0); step(1, 0); step(1, 1);
    step(1, 0); step(0, 0); step(0, 1); step(1, 1);
    CHECK(turns == 1);

    gpio_mock::setInput("GPIO27", 0);
    sleepMs(60);
    gpio_mock::setInput("GPIO27", 1);
    sleepMs(60);
    CHECK(presses == 1);

    encoder::stop();
    CHECK(!gpio_mock::isOpen("GPIO16") && !gpio_mock::isOpen("GPIO27"));
    encoder::stop();
}

// --- Detector ---------------------------------------------------------------

static void testDetector()
{
    const int lineX = W * 6 / 10;
    const double r = 12;
    Zone zone = Zone::None;

    // Straight shot into the goal: exactly one event, only once wholly over.
    {
        detector::Detector d;
        CHECK(roll(d, 40, lineX + r + 30, 30, H / 2, &zone) == 1);
        CHECK(zone == Zone::Goal);
        CHECK(hold(d, lineX + r + 30, 40) == 0);
        CHECK(empty(d, 20) == 0);
    }

    // Zones while approaching.
    {
        detector::Detector d;
        CHECK(d.process(*makeFrame(0, 60)).zone == Zone::Field);
        CHECK(d.process(*makeFrame(0, lineX - r - 2)).zone == Zone::Field);
        CHECK(d.process(*makeFrame(0, lineX)).zone == Zone::Line);
        CHECK(d.process(*makeFrame(0, lineX + r / 2)).zone == Zone::Line);
        detector::Result res = d.process(*makeFrame(0, lineX + r + 3));
        CHECK(res.zone == Zone::Goal && res.found);
        CHECK(std::abs(res.x - (lineX + r + 3)) <= 1 && std::abs(res.y - H / 2) <= 1);
        CHECK(std::abs(res.radius - r) <= 1);
    }

    // Ball stopping on the line, or with its centre over but its edge not.
    {
        detector::Detector d;
        CHECK(roll(d, 40, lineX, 20) == 0);
        CHECK(hold(d, lineX, 30) == 0);
        CHECK(roll(d, lineX, lineX + r - 2, 5) == 0);
        CHECK(hold(d, lineX + r - 2, 30) == 0);
    }

    // Saved just short of the line.
    {
        detector::Detector d;
        CHECK(roll(d, 40, lineX - r - 2, 20) == 0);
        CHECK(roll(d, lineX - r - 2, 40, 20) == 0);
    }

    // Ball that starts in the goal and comes out: wrong direction.
    {
        detector::Detector d;
        CHECK(hold(d, lineX + r + 40, 10) == 0);
        CHECK(roll(d, lineX + r + 40, 60, 20) == 0);
    }

    // A single frame over the line is not enough.
    {
        detector::Detector d;
        CHECK(roll(d, 40, lineX - r - 4, 10) == 0);
        CHECK(d.process(*makeFrame(0, lineX + r + 4)).crossing == false);
        CHECK(hold(d, lineX - r - 4, 5) == 0);
    }

    // Small orange specks are ignored.
    {
        detector::Detector d;
        int specks = 0;
        for (int i = 0; i < 10; ++i)
            specks += d.process(*makeFrame(0, lineX + 40, H / 2, 2)).found;
        CHECK(specks == 0);
    }

    // Two separate shots produce two events.
    {
        detector::Detector d;
        CHECK(roll(d, 40, lineX + r + 30, 30) == 1);
        CHECK(empty(d, 20) == 0);
        CHECK(roll(d, 40, lineX + r + 30, 30) == 1);
    }

    // Ball briefly lost while crossing still counts once.
    {
        detector::Detector d;
        CHECK(roll(d, 40, lineX - r - 4, 20) == 0);
        CHECK(empty(d, 3) == 0);
        CHECK(hold(d, lineX + r + 6, 5) == 1);
    }

    // Calibrated diagonal line with the goal on the left.
    {
        detector::Detector d;
        detector::Line line = {200, 0, 120, H - 1, 20, H / 2};
        CHECK(detector::valid(line, W, H));
        d.setLine(line);

        // At y = 120 the line is at x ~ 160; the ball travels right to left.
        CHECK(roll(d, 290, 200, 20, 120, &zone) == 0);
        CHECK(zone == Zone::Field);
        CHECK(roll(d, 200, 160, 10, 120, &zone) == 0);
        CHECK(zone == Zone::Line);
        CHECK(roll(d, 160, 120, 10, 120, &zone) == 1);
        CHECK(zone == Zone::Goal);
    }

    // Invalid calibrations.
    CHECK(!detector::valid({10, 10, 10, 12, 50, 50}, W, H));
    CHECK(!detector::valid({10, 0, 10, 200, 10, 100}, W, H));
    CHECK(!detector::valid({10, 0, 10, 500, 50, 100}, W, H));
    CHECK(detector::valid(detector::defaultLine(W, H), W, H));
}

// --- Replay -----------------------------------------------------------------

static void testReplay()
{
    replay::configure(15, 3000, 2000);
    CHECK(!replay::current());

    // 100 frames, 66 ms apart.
    for (int i = 0; i <= 100; ++i)
        replay::push(makeFrame(i * 66));

    replay::begin("A", 6600);
    replay::Status s = replay::status();
    CHECK(s.eventId == "A" && s.frames == 46 && !s.complete && !s.playing);
    CHECK(s.frame == 45 && replay::current()->timeMs == 6600);

    // Frames after the event join the clip until the post window ends.
    for (int i = 101; i <= 140; ++i)
        replay::push(makeFrame(i * 66));
    s = replay::status();
    CHECK(s.frames == 46 + 30 && s.complete);
    CHECK(replay::current()->timeMs == 6600);

    for (int i = 0; i < 100; ++i)
        replay::next();
    CHECK(replay::status().frame == 75 && !replay::next());

    CHECK(replay::seek(10) && replay::status().frame == 10);
    CHECK(!replay::seek(-1) && !replay::seek(76) && !replay::seek(10));
    for (int i = 0; i < 20; ++i)
        replay::previous();
    CHECK(replay::status().frame == 0 && !replay::previous());

    // Play, pause, resume.
    CHECK(replay::setPlaying(true) && !replay::setPlaying(true));
    long long t = 100000;
    CHECK(replay::tick(t) && replay::status().frame == 1);
    CHECK(!replay::tick(t + 30) && replay::status().frame == 1);
    CHECK(replay::tick(t + 70) && replay::status().frame == 2);
    CHECK(replay::setPlaying(false));
    CHECK(!replay::tick(t + 500) && replay::status().frame == 2);
    CHECK(replay::togglePlayback() && replay::status().playing);
    for (int i = 1; i <= 100; ++i)
        replay::tick(t + 1000 + i * 70);
    s = replay::status();
    CHECK(s.frame == 75 && !s.playing);

    // Playing again from the end restarts the clip.
    CHECK(replay::togglePlayback() && replay::status().frame == 0);
    replay::setPlaying(false);

    // A new event gets its own clip.
    replay::begin("B", 140 * 66);
    s = replay::status();
    CHECK(s.eventId == "B" && s.frames > 0 && !s.complete);
    CHECK(replay::current()->timeMs == 140 * 66);
    replay::previous();
    replay::seek(0);
    CHECK(replay::current()->timeMs >= 140 * 66 - 3000);

    // Playback waits at the end of a clip that is still recording.
    replay::seek(s.frames - 1);
    replay::setPlaying(true);
    replay::tick(140 * 66 + 60);
    CHECK(replay::status().playing);
    replay::push(makeFrame(141 * 66));
    replay::tick(140 * 66 + 160);
    CHECK(replay::status().frame == s.frames);

    replay::clear();
    s = replay::status();
    CHECK(s.eventId.empty() && s.frames == 0 && !replay::current());
    CHECK(!replay::next() && !replay::togglePlayback());

    // No camera frames at all.
    replay::configure(15, 3000, 2000);
    replay::begin("C", 5000);
    s = replay::status();
    CHECK(s.frames == 0 && !replay::current() && !replay::togglePlayback());
    replay::tick(5000 + 2600);
    CHECK(replay::status().complete);

    // Frames that arrive long after the event are not added.
    replay::push(makeFrame(20000));
    CHECK(replay::status().frames == 0);
}

// --- Controller -------------------------------------------------------------

struct Sent {
    std::vector<std::string> text;
    std::vector<std::pair<int, std::string>> frames;

    void clear()
    {
        text.clear();
        frames.clear();
    }

    int count(const std::string& prefix) const
    {
        int n = 0;
        for (auto& t : text)
            n += startsWith(t, prefix);
        return n;
    }

    std::string last(const std::string& prefix) const
    {
        for (auto it = text.rbegin(); it != text.rend(); ++it)
            if (startsWith(*it, prefix))
                return *it;
        return "";
    }
};

static Sent sent;

static void checkLeds(int green, int yellow, int red)
{
    CHECK(gpio_mock::value("GPIO5") == green);
    CHECK(gpio_mock::value("GPIO6") == yellow);
    CHECK(gpio_mock::value("GPIO26") == red);
}

static long long feedFrames(long long from, long long to)
{
    for (long long t = from; t <= to; t += 66)
    {
        auto f = makeFrame(t, 50);
        f->ball = true;
        f->ballX = 50;
        f->ballY = 120;
        f->ballR = 12;
        f->zone = Zone::Field;
        camera::FramePtr frame = f;
        replay::push(frame);
        controller::cameraFrame(frame);
    }
    return to;
}

static void startController()
{
    gpio_mock::reset();
    CHECK(leds::init());
    CHECK(buzzer::init());
    replay::configure(15, 3000, 2000);
    sent.clear();

    controller::init(
        [](const std::string& line) { sent.text.push_back(line); },
        [](int slot, const std::string& header, controller::Bytes data) {
            CHECK(data && !data->empty());
            sent.frames.push_back({slot, header});
        },
        8);
    controller::setCaptureHook(nullptr);
}

// Each whistle is three blasts; waits for any whistle in progress to finish.
static int blasts()
{
    bool waited = false;
    for (int i = 0; i < 400 && buzzer::active(); ++i, waited = true)
        sleepMs(5);
    if (waited)
        sleepMs(30);   // a gap, so the mock counts the next whistle's blasts separately
    return int(gpio_mock::burstLog("GPIO25").size());
}

static goalcam::Evidence evidenceOf(const char* result)
{
    goalcam::Evidence e;
    e.result = result;
    return e;
}

using controller::State;

// Manual reviews, LEDs, buzzer and replay controls, without the AI camera.
static void testController()
{
    startController();

    // All LEDs off at startup and while monitoring.
    checkLeds(0, 0, 0);
    CHECK(controller::state() == State::Monitoring);

    // Live frames are throttled to the preview rate.
    feedFrames(0, 66 * 15);
    CHECK(sent.frames.size() == 8);
    CHECK(sent.frames[0].first == controller::LiveSlot);
    CHECK(startsWith(sent.frames[0].second, "FRAME LIVE - 0 320 240 50 120 12 1 230400"));

    controller::setCameraStatus("sim");
    controller::tick(1000);
    CHECK(sent.last("STATUS") == "STATUS monitoring sim offline -");

    // The encoder and replay commands do nothing outside a review.
    controller::turn(1);
    controller::press();
    controller::message("REPLAY NEXT", 1000);
    controller::tick(1010);
    CHECK(sent.count("REPLAY") == 0);

    // The camera alone never opens an event.
    sent.clear();
    controller::detected(controller::Source::Camera, 990, 1000);
    CHECK(controller::state() == State::Monitoring && controller::eventId().empty());
    CHECK(sent.count("EVENT") == 0);
    checkLeds(0, 0, 0);

    // Simulate Event: Awaiting Decision with the yellow LED at once.
    controller::message("REVIEW", 1000);
    std::string id = controller::eventId();
    CHECK(!id.empty() && controller::state() == State::Awaiting);
    CHECK(sent.last("EVENT") == "EVENT " + id + " manual");
    CHECK(sent.last("AWAITING") == "AWAITING " + id + " manual");
    checkLeds(0, 1, 0);

    // Repeated clicks do not open another review.
    controller::message("REVIEW", 1010);
    CHECK(controller::eventId() == id && sent.count("EVENT " + id) == 2);

    // No live frames during the review; post-event frames join the replay.
    sent.frames.clear();
    feedFrames(1056, 1056 + 66 * 5);
    CHECK(sent.frames.empty());

    controller::tick(1500);
    CHECK(sent.last("STATUS") == "STATUS awaiting sim offline " + id);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 15 22 0 0");
    CHECK(sent.frames.size() == 1 && sent.frames[0].first == controller::ReplaySlot);

    // An IR trigger joins the manual event but never makes it automatic.
    controller::detected(controller::Source::Ir, 900, 1900);
    controller::detected(controller::Source::Ir, 900, 2000);
    CHECK(sent.last("EVENT") == "EVENT " + id + " manual,ir");
    CHECK(controller::state() == State::Awaiting && controller::eventId() == id);

    // Encoder steps and dashboard commands move the same replay.
    sent.clear();
    controller::turn(1);
    controller::tick(1600);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 16 22 0 0");
    CHECK(sent.frames.size() == 1 && startsWith(sent.frames[0].second, "FRAME REPLAY " + id + " 16 "));
    for (int i = 0; i < 30; ++i)
        controller::turn(1);
    controller::tick(1610);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 21 22 0 0");
    controller::turn(-1);
    controller::tick(1615);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 20 22 0 0");
    controller::message("REPLAY SEEK 3", 1620);
    controller::message("REPLAY SEEK 99", 1630);
    controller::message("REPLAY BOGUS", 1630);
    controller::tick(1630);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 3 22 0 0");
    controller::press();
    controller::tick(1700);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 4 22 1 0");
    controller::message("REPLAY PAUSE", 1750);
    controller::tick(1800);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 4 22 0 0");

    // Reconnecting resends the pending event, the reason and the replay.
    sent.clear();
    controller::resync();
    controller::tick(1840);
    CHECK(sent.count("EVENT " + id) == 1 && sent.count("AWAITING " + id + " manual") == 1);
    CHECK(sent.count("STATUS") == 1 && sent.count("REPLAY") == 1 && sent.frames.size() == 1);

    // Yellow stays on while the referee takes minutes over it.
    controller::tick(1000 + 5 * 60 * 1000);
    CHECK(controller::state() == State::Awaiting);
    checkLeds(0, 1, 0);
    CHECK(blasts() == 0);

    // Invalid and stale decisions are ignored.
    long long t = 400000;
    controller::message("DECISION wrong-id GOAL", t);
    controller::message("DECISION " + id + " MAYBE", t);
    controller::message("DECISION", t);
    controller::message("garbage", t);
    CHECK(controller::state() == State::Awaiting);
    checkLeds(0, 1, 0);

    // Confirm: yellow off, green for 3 s, the whistle once.
    controller::message("DECISION " + id + " GOAL", t);
    checkLeds(1, 0, 0);
    CHECK(controller::state() == State::Goal && buzzer::active());
    controller::message("DECISION " + id + " GOAL", t + 100);
    controller::message("DECISION " + id + " NO_GOAL", t + 100);
    checkLeds(1, 0, 0);
    CHECK(blasts() == 3);
    controller::tick(t + 2990);
    checkLeds(1, 0, 0);
    CHECK(controller::state() == State::Goal);
    controller::tick(t + 3000);
    checkLeds(0, 0, 0);
    CHECK(controller::state() == State::Monitoring);

    // Reject: yellow off, red for 3 s, no sound.
    t += 10000;
    controller::message("REVIEW", t);
    std::string manual = controller::eventId();
    CHECK(manual != id);
    checkLeds(0, 1, 0);
    controller::message("DECISION " + manual + " NO_GOAL", t + 100);
    checkLeds(0, 0, 1);
    CHECK(controller::state() == State::NoGoal && !buzzer::active());
    controller::tick(t + 3090);
    checkLeds(0, 0, 1);
    controller::tick(t + 3100);
    checkLeds(0, 0, 0);
    CHECK(controller::state() == State::Monitoring && blasts() == 3);

    // An IR event without the AI camera waits for the referee.
    t += 10000;
    controller::detected(controller::Source::Ir, t - 1000, t);
    std::string ir = controller::eventId();
    CHECK(controller::state() == State::Awaiting);
    CHECK(sent.last("AWAITING") == "AWAITING " + ir + " camera_unavailable");
    checkLeds(0, 1, 0);

    // Sensor and camera faults do not change the LEDs.
    controller::setIrStatus("error");
    controller::setCameraStatus("offline");
    controller::tick(t + 100);
    checkLeds(0, 1, 0);
    controller::setIrStatus("ok");
    controller::setCameraStatus("sim");
    controller::message("DECISION " + ir + " NO_GOAL", t + 200);
    controller::tick(t + 3300);
    checkLeds(0, 0, 0);

    // Several manual goals in a row: each whistles once, with its own id.
    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i)
    {
        t += 10000;
        controller::message("REVIEW", t);
        ids.push_back(controller::eventId());
        controller::message("DECISION " + ids.back() + " GOAL", t + 100);
        blasts();
        controller::tick(t + 3200);
        CHECK(controller::state() == State::Monitoring);
    }
    CHECK(blasts() == 12);
    CHECK(ids[0] != ids[1] && ids[1] != ids[2]);

    // A new review during the green notification: green off, yellow on.
    t += 10000;
    controller::message("REVIEW", t);
    controller::message("DECISION " + controller::eventId() + " GOAL", t);
    controller::message("REVIEW", t + 100);
    checkLeds(0, 1, 0);
    blasts();

    // Status heartbeat.
    sent.clear();
    controller::tick(t + 200);
    controller::tick(t + 300);
    controller::tick(t + 1300);
    CHECK(sent.count("STATUS") == 2);

    // Shutdown while awaiting a decision: every LED and the buzzer off.
    buzzer::cleanup();
    leds::cleanup();
    checkLeds(0, 0, 0);
    CHECK(gpio_mock::value("GPIO25") == 0);
    CHECK(!gpio_mock::isOpen("GPIO25") && !gpio_mock::isOpen("GPIO5") && !gpio_mock::isOpen("GPIO6"));

    buzzer::cleanup();
    leds::cleanup();
    buzzer::whistle();
    leds::show(leds::Color::Red);
}

// IR events judged by camera evidence.
static void testHybrid()
{
    startController();
    controller::setCameraStatus("yolo");

    std::vector<std::pair<std::string, int>> captures;
    bool cameraAnswers = true;
    controller::setCaptureHook([&](const std::string& id, int offset) {
        captures.push_back({id, offset});
        return cameraAnswers;
    });

    // IR + confident, event-correlated detection: automatic goal.
    long long t = 10000;
    controller::detected(controller::Source::Ir, t - 1000, t);
    std::string id = controller::eventId();
    CHECK(captures.size() == 1 && captures[0].first == id && captures[0].second == 1000);
    CHECK(controller::state() == State::Evaluating);
    CHECK(sent.last("CAMERA") == "CAMERA " + id + " pending pending");
    checkLeds(0, 0, 0);

    // Repeated triggers during the evaluation stay in the same event.
    controller::detected(controller::Source::Ir, t - 500, t + 500);
    CHECK(controller::eventId() == id && sent.count("EVENT") == 1);

    // Evidence for another event is ignored.
    controller::cameraEvidence("other", evidenceOf("confident"), "pending", t + 1000);
    CHECK(controller::state() == State::Evaluating);

    goalcam::Evidence confident = evidenceOf("confident");
    confident.measured = true;
    confident.runs = 6;
    confident.ballRuns = 3;
    confident.best = 0.914;
    confident.firstMs = -900;
    confident.lastMs = 100;
    controller::cameraEvidence(id, confident, "pending", t + 2000);
    CHECK(controller::state() == State::Goal);
    CHECK(sent.last("AUTO") == "AUTO " + id + " ir");
    CHECK(sent.last("CAMERA") == "CAMERA " + id + " confident pending");
    CHECK(sent.last("CONFIDENCE") == "CONFIDENCE " + id + " 6 3 91 -900 100 0");
    checkLeds(1, 0, 0);
    CHECK(blasts() == 3);

    // The replay arrives after the decision and is kept for the referee.
    std::vector<camera::FramePtr> clip;
    for (int i = 0; i < 8; ++i)
    {
        auto f = std::make_shared<camera::Frame>();
        f->width = 640;
        f->height = 480;
        f->jpeg = std::vector<uint8_t>{0xff, 0xd8, uint8_t(i), 0xff, 0xd9};
        clip.push_back(f);
    }
    controller::cameraClip(id, clip, 10);
    CHECK(sent.last("CAMERA") == "CAMERA " + id + " confident ready");
    controller::tick(t + 5100);
    CHECK(controller::state() == State::Monitoring);
    checkLeds(0, 0, 0);

    // The referee replays it from the dashboard, with the encoder too.
    sent.clear();
    controller::message("VIEW " + id, t + 6000);
    controller::tick(t + 6010);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 0 8 0 1" && sent.frames.size() == 1);
    controller::turn(1);
    controller::tick(t + 6020);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 1 8 0 1");
    controller::message("VIEW -", t + 6030);
    controller::turn(1);
    controller::tick(t + 6040);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 1 8 0 1");

    // Until the server confirms it, a reconnect sends the automatic goal again.
    sent.clear();
    controller::resync();
    CHECK(sent.count("AUTO " + id) == 1);
    controller::message("DECISION " + id + " GOAL", t + 6100);
    sent.clear();
    controller::resync();
    CHECK(sent.count("AUTO") == 0);
    CHECK(blasts() == 3);

    // Correction to No Goal: red for 3 s, no whistle, only once.
    controller::message("DECISION " + id + " NO_GOAL", t + 7000);
    checkLeds(0, 0, 1);
    CHECK(controller::state() == State::NoGoal);
    controller::tick(t + 10000);
    checkLeds(0, 0, 0);
    controller::message("DECISION " + id + " NO_GOAL", t + 11000);
    checkLeds(0, 0, 0);
    CHECK(controller::state() == State::Monitoring && blasts() == 3);

    // Every uncertain answer waits for the referee, with the reason; no sound.
    const char* uncertain[][2] = {
        {"none", "no_ball"}, {"missed", "no_ball"}, {"low", "low_confidence"},
        {"stationary", "ball_not_moving"},
        {"insufficient", "insufficient"}, {"seen", "unverified"},
        {"ignored", "camera_busy"}, {"unavailable", "camera_unavailable"},
    };
    for (auto& u : uncertain)
    {
        t += 20000;
        controller::detected(controller::Source::Ir, t - 1000, t);
        std::string e = controller::eventId();
        checkLeds(0, 0, 0);
        controller::cameraEvidence(e, evidenceOf(u[0]), "pending", t + 1500);
        CHECK(controller::state() == State::Awaiting);
        CHECK(sent.last("AWAITING") == "AWAITING " + e + " " + u[1]);
        checkLeds(0, 1, 0);

        // Late evidence cannot overturn it into an automatic goal.
        controller::cameraEvidence(e, confident, "ready", t + 2000);
        CHECK(controller::state() == State::Awaiting && sent.count("AUTO " + e) == 0);
        controller::message("DECISION " + e + " NO_GOAL", t + 60000);
        checkLeds(0, 0, 1);
        controller::tick(t + 63000);
    }
    CHECK(blasts() == 3);

    // No answer in time: Awaiting Decision.
    t += 100000;
    controller::detected(controller::Source::Ir, t - 1000, t);
    std::string slow = controller::eventId();
    controller::tick(t + 7990);
    CHECK(controller::state() == State::Evaluating);
    controller::tick(t + 8000);
    CHECK(controller::state() == State::Awaiting);
    CHECK(sent.last("AWAITING") == "AWAITING " + slow + " timeout");
    checkLeds(0, 1, 0);
    controller::message("DECISION " + slow + " GOAL", t + 9000);
    CHECK(blasts() == 6);
    controller::tick(t + 12000);

    // Stale evidence from the previous event does not touch the new one.
    t += 20000;
    controller::detected(controller::Source::Ir, t - 1000, t);
    controller::cameraEvidence(slow, confident, "ready", t + 100);
    CHECK(controller::state() == State::Evaluating);
    controller::message("DECISION " + controller::eventId() + " NO_GOAL", t + 200);
    controller::tick(t + 3200);

    // Camera offline, or the trigger cannot be sent: straight to the referee.
    t += 20000;
    controller::setCameraStatus("offline");
    controller::detected(controller::Source::Ir, t - 1000, t);
    CHECK(sent.last("AWAITING") == "AWAITING " + controller::eventId() + " camera_unavailable");
    checkLeds(0, 1, 0);
    controller::message("DECISION " + controller::eventId() + " NO_GOAL", t + 100);
    controller::tick(t + 3100);
    controller::setCameraStatus("yolo");
    cameraAnswers = false;
    t += 20000;
    controller::detected(controller::Source::Ir, t - 1000, t);
    CHECK(sent.last("CAMERA") == "CAMERA " + controller::eventId() + " unavailable unavailable");
    CHECK(sent.last("AWAITING") == "AWAITING " + controller::eventId() + " camera_unavailable");
    controller::message("DECISION " + controller::eventId() + " NO_GOAL", t + 100);
    controller::tick(t + 3100);
    cameraAnswers = true;

    // Simulate Event never becomes automatic, even with confident evidence.
    t += 20000;
    controller::message("REVIEW", t);
    std::string manual = controller::eventId();
    controller::cameraEvidence(manual, confident, "pending", t + 1500);
    CHECK(controller::state() == State::Awaiting && sent.count("AUTO " + manual) == 0);
    checkLeds(0, 1, 0);
    controller::message("DECISION " + manual + " GOAL", t + 5000);
    checkLeds(1, 0, 0);
    CHECK(blasts() == 9);

    // A correction for an older automatic goal is ignored once a new one exists.
    controller::tick(t + 8000);
    t += 20000;
    controller::detected(controller::Source::Ir, t - 1000, t);
    std::string second = controller::eventId();
    controller::cameraEvidence(second, confident, "pending", t + 1500);
    CHECK(sent.last("AUTO") == "AUTO " + second + " ir" && blasts() == 12);
    controller::tick(t + 5000);
    controller::message("DECISION " + id + " NO_GOAL", t + 6000);
    checkLeds(0, 0, 0);

    controller::setCaptureHook(nullptr);
    buzzer::cleanup();
    leds::cleanup();
}

static goalcam::Inference ball(int dtMs, double confidence, int x, int y)
{
    goalcam::Inference run{dtMs, confidence};
    run.hasPosition = true;
    run.x = x;
    run.y = y;
    return run;
}

static void testEvaluate()
{
    // Runs are listed relative to the trigger; the beam broke 1 s earlier.
    auto judge = [](std::vector<goalcam::Inference> runs) { return goalcam::evaluate(runs, 1000); };

    // A moving ball in two confident runs near the beam break.
    goalcam::Evidence e = judge({{-2100, 0}, ball(-1800, 0.91, 200, 240), ball(-1500, 0.82, 290, 250),
                                 {-1200, 0}, ball(-900, 0.4, 400, 260)});
    CHECK(e.result == "confident" && e.measured);
    CHECK(e.runs == 5 && e.ballRuns == 2 && std::abs(e.best - 0.91) < 1e-9);
    CHECK(e.firstMs == -800 && e.lastMs == 100 && e.movePx == 90);

    // The same ball standing still (e.g. a spare ball in view) is not a goal.
    e = judge({ball(-1800, 0.95, 300, 240), ball(-1500, 0.95, 310, 245), ball(-1200, 0.95, 305, 240)});
    CHECK(e.result == "stationary" && e.movePx < 40);
    // Without positions the movement cannot be checked either.
    CHECK(judge({{-1800, 0.95}, {-1500, 0.95}, {-1200, 0}}).result == "stationary");

    // One confident run listed twice counts once.
    CHECK(judge({ball(-1800, 0.91, 100, 100), ball(-1800, 0.91, 100, 100), ball(-1500, 0.3, 300, 100),
                 {-1200, 0}}).result == "low");
    // Just under the 75% threshold.
    CHECK(judge({ball(-1800, 0.749, 100, 100), ball(-1500, 0.749, 200, 100), ball(-1200, 0.749, 300, 100)})
              .result == "low");
    CHECK(judge({{-1800, 0}, {-1500, 0}, {-1200, 0}, {-900, 0}}).result == "none");
    // Too few runs to judge, even if both saw a moving ball.
    CHECK(judge({ball(-1800, 0.95, 100, 100), ball(-1500, 0.95, 300, 100)}).result == "insufficient");
    CHECK(judge({}).result == "insufficient");

    // Window edges: 1.5 s before to 1 s after the beam break (here -2500 to 0 ms from the trigger).
    e = judge({ball(-2501, 0.99, 100, 100), {-2500, 0}, {-1200, 0}, {0, 0}, ball(1, 0.99, 400, 100)});
    CHECK(e.result == "none" && e.runs == 3);

    // Detections long before or after the beam break do not count.
    e = judge({ball(-4200, 0.99, 100, 100), ball(-3900, 0.99, 300, 100), {-1500, 0}, {-1200, 0},
               {-900, 0}, ball(1500, 0.99, 500, 100)});
    CHECK(e.result == "none" && e.runs == 3 && e.ballRuns == 0);

    // A manual review has no beam break (offset 0): the window is around the trigger.
    e = goalcam::evaluate({ball(-1400, 0.9, 100, 100), ball(-1000, 0.9, 200, 100), {-600, 0},
                           ball(900, 0.9, 300, 100)}, 0);
    CHECK(e.result == "confident" && e.runs == 4 && e.ballRuns == 3);

    std::vector<goalcam::Inference> runs;
    CHECK(goalcam::parseEvidence("EVIDENCE -1200:0.000 -900:0.880:320:241 0:1.000:5:6", runs));
    CHECK(runs.size() == 3 && runs[1].dtMs == -900 && std::abs(runs[1].confidence - 0.88) < 1e-9);
    CHECK(!runs[0].hasPosition && runs[1].hasPosition && runs[1].x == 320 && runs[1].y == 241);
    CHECK(goalcam::parseEvidence("EVIDENCE", runs) && runs.empty());
    CHECK(!goalcam::parseEvidence("EVIDENCE -10:0.5 x", runs));
    CHECK(!goalcam::parseEvidence("EVIDENCE 5:1.5", runs));
    CHECK(!goalcam::parseEvidence("EVIDENCE 5:0.5ms", runs));
    CHECK(!goalcam::parseEvidence("EVIDENCE 5:0.5:3", runs));
    CHECK(!goalcam::parseEvidence("EVIDENCE 5:0.5:3:4:5", runs));
    CHECK(!goalcam::parseEvidence("EVIDENCE 5:0.5:", runs));
    CHECK(!goalcam::parseEvidence("cam 15 fps", runs));
}

static void checkBlast(const gpio_mock::Burst& b, int ms)
{
    const int hz = 2000;
    double lengthMs = (b.endUs - b.startUs) / 1000.0;
    double duty = 100.0 * b.highUs / (b.cycles * 1e6 / hz);
    std::cout << "Buzzer blast: " << b.cycles << " cycles in " << int(lengthMs) << " ms ("
              << int(b.cycles * 1000 / std::max(1.0, lengthMs)) << " Hz), duty " << int(duty + 0.5) << "%\n";

    CHECK(lengthMs >= ms - 10 && lengthMs <= ms + 2);
    CHECK(b.cycles >= hz * ms / 1000 * 95 / 100 && b.cycles <= hz * ms / 1000 + 1);
    CHECK(duty >= 20 && duty <= 32);
}

static void testBuzzerWhistle()
{
    using std::chrono::steady_clock;
    gpio_mock::reset();

    // Silent at startup and while idle.
    CHECK(buzzer::init());
    sleepMs(200);
    CHECK(gpio_mock::value("GPIO25") == 0 && gpio_mock::rises("GPIO25") == 0 && !buzzer::active());

    // whistle() returns at once: 200 ms on, 150 off, 200 on, 150 off, 600 on, same pitch.
    auto start = steady_clock::now();
    buzzer::whistle();
    CHECK(steady_clock::now() - start < std::chrono::milliseconds(5));
    CHECK(buzzer::active());
    CHECK(blasts() == 3);
    long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(steady_clock::now() - start).count();
    std::cout << "Whistle total: " << totalMs << " ms\n";
    CHECK(totalMs >= 1290 && totalMs <= 1400);

    std::vector<gpio_mock::Burst> log = gpio_mock::burstLog("GPIO25");
    if (log.size() == 3)
    {
        checkBlast(log[0], 200);
        checkBlast(log[1], 200);
        checkBlast(log[2], 600);
        // Silent between the blasts.
        for (int i = 1; i < 3; ++i)
        {
            long long gapMs = (log[i].startUs - log[i - 1].endUs) / 1000;
            CHECK(gapMs >= 145 && gapMs <= 170);
        }
    }
    CHECK(gpio_mock::value("GPIO25") == 0 && !buzzer::active());

    // Repeated requests while it plays neither overlap nor restart it.
    sleepMs(50);
    buzzer::whistle();
    sleepMs(300);
    buzzer::whistle();
    sleepMs(500);
    buzzer::whistle();
    CHECK(blasts() == 6);
    log = gpio_mock::burstLog("GPIO25");
    CHECK(log[5].endUs - log[3].startUs < 1400000);

    // A later, separate goal sounds again.
    sleepMs(50);
    buzzer::whistle();
    CHECK(blasts() == 9);

    // Shutdown during a blast, or during a pause, stops at once with the pin low.
    sleepMs(50);
    buzzer::whistle();
    sleepMs(100);
    start = steady_clock::now();
    buzzer::cleanup();
    CHECK(steady_clock::now() - start < std::chrono::milliseconds(20));
    CHECK(gpio_mock::value("GPIO25") == 0 && !gpio_mock::isOpen("GPIO25"));
    CHECK(gpio_mock::burstLog("GPIO25").size() == 10);

    gpio_mock::reset();
    CHECK(buzzer::init());
    buzzer::whistle();
    sleepMs(270);
    start = steady_clock::now();
    buzzer::cleanup();
    CHECK(steady_clock::now() - start < std::chrono::milliseconds(20));
    CHECK(gpio_mock::burstLog("GPIO25").size() == 1 && gpio_mock::value("GPIO25") == 0);

    // No sound after shutdown or before init.
    buzzer::whistle();
    CHECK(!buzzer::active());
    buzzer::cleanup();

    // An unavailable buzzer line is reported, not hidden.
    gpio_mock::setBroken("GPIO25", true);
    CHECK(!buzzer::init());
    buzzer::whistle();
    CHECK(!buzzer::active());
    gpio_mock::reset();
}

static void testLeds()
{
    gpio_mock::reset();
    CHECK(leds::init());
    checkLeds(0, 0, 0);

    // Only one LED at a time.
    CHECK(leds::show(leds::Color::Green));
    checkLeds(1, 0, 0);
    CHECK(leds::show(leds::Color::Yellow));
    checkLeds(0, 1, 0);
    CHECK(leds::show(leds::Color::Red));
    checkLeds(0, 0, 1);
    CHECK(leds::show(leds::Color::Off));
    checkLeds(0, 0, 0);

    // A write failure is reported.
    gpio_mock::setBroken("GPIO6", true);
    CHECK(!leds::show(leds::Color::Yellow));
    gpio_mock::setBroken("GPIO6", false);
    leds::cleanup();
    checkLeds(0, 0, 0);
    CHECK(!gpio_mock::isOpen("GPIO5") && !gpio_mock::isOpen("GPIO6") && !gpio_mock::isOpen("GPIO26"));

    // One missing LED does not disable the others.
    gpio_mock::reset();
    gpio_mock::setBroken("GPIO26", true);
    CHECK(!leds::init());
    CHECK(leds::show(leds::Color::Yellow));
    CHECK(gpio_mock::value("GPIO6") == 1 && gpio_mock::value("GPIO5") == 0);
    leds::cleanup();
    gpio_mock::reset();
}

// --- IR sensors and pins -----------------------------------------------------

static void testBeamCheck()
{
    sensors::BeamCheck check;
    int clear[3] = {1, 1, 1};
    int beam2[3] = {1, 0, 1};

    CHECK(check.update(clear, 0) == -1);

    // Broken, then clear 1 s later: a goal on that beam.
    CHECK(check.update(beam2, 100) == -1);
    CHECK(check.update(clear, 600) == -1);
    CHECK(check.update(clear, 1099) == -1);
    CHECK(check.update(clear, 1100) == 1);
    CHECK(check.brokenAt() == 100);
    CHECK(check.update(clear, 1200) == -1);

    // Still blocked after 1 s: no goal until it clears and breaks again.
    CHECK(check.update(beam2, 2000) == -1);
    CHECK(check.update(beam2, 3000) == -1);
    CHECK(check.update(clear, 3010) == -1);
    CHECK(check.update(clear, 3100) == -1);
    CHECK(check.update(beam2, 3200) == -1);
    CHECK(check.update(clear, 4200) == 1);

    // A beam blocked for over 5 s is ignored until it clears; the others still work.
    int stuck[3] = {0, 1, 1};
    int stuckAnd3[3] = {0, 1, 0};
    CHECK(check.update(stuck, 5000) == -1);
    CHECK(check.update(stuck, 6000) == -1);
    CHECK(check.update(stuck, 10001) == -1);
    CHECK(check.stuck(0));
    CHECK(check.update(stuckAnd3, 10100) == -1);
    CHECK(check.update(stuck, 11100) == 2);
    CHECK(check.update(clear, 11200) == -1);
    CHECK(!check.stuck(0));

    // Unavailable beams never trigger.
    int missing[3] = {-1, -1, 1};
    for (int t = 12000; t < 15000; t += 100)
        CHECK(check.update(missing, t) == -1);
}

static void testIrThread()
{
    std::mutex m;
    std::vector<std::string> statuses;
    std::atomic<int> goals{0};
    auto onGoal = [&](long long broken, long long now) { CHECK(now - broken >= 1000); ++goals; };
    auto onStatus = [&](const std::string& s) { std::lock_guard<std::mutex> l(m); statuses.push_back(s); };
    auto last = [&] { std::lock_guard<std::mutex> l(m); return statuses.empty() ? "" : statuses.back(); };

    // GPIO18 unavailable: the other two still detect, and the status says degraded.
    gpio_mock::reset();
    gpio_mock::setBroken("GPIO18", true);
    CHECK(sensors::start({}, onGoal, onStatus));
    sleepMs(50);
    CHECK(last() == "degraded");
    gpio_mock::setInput("GPIO23", 0);
    sleepMs(100);
    gpio_mock::setInput("GPIO23", 1);
    sleepMs(1100);
    CHECK(goals == 1);

    // A sensor failing while running is dropped once, without stopping the rest.
    gpio_mock::setBroken("GPIO24", true);
    sleepMs(50);
    gpio_mock::setInput("GPIO23", 0);
    sleepMs(100);
    gpio_mock::setInput("GPIO23", 1);
    sleepMs(1100);
    CHECK(goals == 2 && last() == "degraded");

    sensors::stop();
    CHECK(!gpio_mock::isOpen("GPIO23") && !gpio_mock::isOpen("GPIO24"));
    sensors::stop();

    // Skipping the broken sensor: the remaining ones are all healthy.
    gpio_mock::reset();
    gpio_mock::setBroken("GPIO18", true);
    CHECK(sensors::start({"GPIO18"}, onGoal, onStatus));
    sleepMs(50);
    CHECK(last() == "ok");
    sensors::stop();

    // None working, or all skipped.
    gpio_mock::reset();
    for (const char* pin : pins::kIrBeams)
        gpio_mock::setBroken(pin, true);
    CHECK(sensors::start({}, onGoal, onStatus));
    sleepMs(50);
    CHECK(last() == "error");
    sensors::stop();
    CHECK(!sensors::start({"GPIO18", "GPIO23", "GPIO24"}, onGoal, onStatus));
    CHECK(last() == "disabled");
    gpio_mock::reset();
}

static void testPinsDistinct()
{
    std::vector<std::string> all = {pins::kEncoderA, pins::kEncoderB, pins::kEncoderButton,
                                    pins::kLedGreen, pins::kLedYellow, pins::kLedRed, pins::kBuzzer};
    all.insert(all.end(), std::begin(pins::kIrBeams), std::end(pins::kIrBeams));
    CHECK(all.size() == 10 && std::string(pins::kLedYellow) == "GPIO6");
    std::sort(all.begin(), all.end());
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
}

// --- Camera -----------------------------------------------------------------

static void testCamera()
{
    std::mutex m;
    std::vector<std::string> statuses;
    std::atomic<int> frames{0};
    std::atomic<bool> sizeOk{true};

    camera::Config config;
    config.simulate = true;
    config.width = 160;
    config.height = 120;
    config.fps = 20;

    camera::start(config,
        [&](std::shared_ptr<camera::Frame> f) {
            if (f->width != 160 || f->height != 120 || f->rgb.size() != 160 * 120 * 3)
                sizeOk = false;
            ++frames;
        },
        [&](const std::string& s) {
            std::lock_guard<std::mutex> lock(m);
            statuses.push_back(s);
        });

    sleepMs(1000);
    camera::stop();
    int count = frames;
    CHECK(count >= 17 && count <= 22);
    CHECK(sizeOk);
    CHECK(statuses.size() == 2 && statuses[0] == "sim" && statuses[1] == "offline");

    // A missing camera reports offline and still stops promptly.
    statuses.clear();
    frames = 0;
    config.simulate = false;
    config.device = "/dev/true-goal-missing";

    camera::start(config, [&](std::shared_ptr<camera::Frame>) { ++frames; },
        [&](const std::string& s) {
            std::lock_guard<std::mutex> lock(m);
            statuses.push_back(s);
        });

    sleepMs(300);
    auto begin = std::chrono::steady_clock::now();
    camera::stop();
    auto took = std::chrono::steady_clock::now() - begin;

    CHECK(frames == 0 && !statuses.empty() && statuses[0] == "offline");
    CHECK(took < std::chrono::milliseconds(500));
}

// --- Network ----------------------------------------------------------------

static int listenLocal(int& port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(fd, 4);

    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    return fd;
}

static int acceptWithin(int listener, int ms)
{
    pollfd p = {listener, POLLIN, 0};
    return poll(&p, 1, ms) == 1 ? accept(listener, nullptr, nullptr) : -1;
}

static bool readExactly(int fd, char* out, size_t n, int ms)
{
    size_t got = 0;
    while (got < n)
    {
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, ms) != 1)
            return false;
        ssize_t r = recv(fd, out + got, n - got, 0);
        if (r <= 0)
            return false;
        got += r;
    }
    return true;
}

static std::string readLine(int fd, int ms)
{
    std::string line;
    char c;
    while (readExactly(fd, &c, 1, ms) && c != '\n')
        line += c;
    return line;
}

static void testNetwork()
{
    int port = 0;
    int listener = listenLocal(port);

    std::mutex m;
    std::vector<std::string> received;
    std::atomic<int> connects{0};

    network::start("127.0.0.1", port,
        [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(m);
            received.push_back(line);
        },
        [&] { ++connects; network::send("HELLO"); });

    int conn = acceptWithin(listener, 3000);
    CHECK(conn >= 0);
    sleepMs(100);
    CHECK(connects == 1 && network::connected());
    CHECK(network::localAddress() == "127.0.0.1");
    CHECK(readLine(conn, 1000) == "HELLO");

    const char* msgs = "PING\nREVIEW\r\nDECISION a GO";
    send(conn, msgs, std::strlen(msgs), 0);
    send(conn, "AL\n", 3, 0);
    sleepMs(200);
    {
        std::lock_guard<std::mutex> lock(m);
        CHECK(received.size() == 3 && received[1] == "REVIEW" && received[2] == "DECISION a GOAL");
    }

    // A large frame arrives intact, after text queued before it.
    auto data = std::make_shared<std::vector<uint8_t>>(300000);
    for (size_t i = 0; i < data->size(); ++i)
        (*data)[i] = uint8_t(i * 7);
    network::send("BEFORE");
    network::sendFrame(1, "FRAME TEST", data);
    CHECK(readLine(conn, 1000) == "BEFORE");
    CHECK(readLine(conn, 1000) == "FRAME TEST");
    std::vector<char> body(data->size());
    CHECK(readExactly(conn, body.data(), body.size(), 2000));
    CHECK(std::memcmp(body.data(), data->data(), body.size()) == 0);

    // Server closes the connection: the board reconnects.
    close(conn);
    conn = acceptWithin(listener, 4000);
    CHECK(conn >= 0);
    sleepMs(100);
    CHECK(connects == 2);
    CHECK(readLine(conn, 1000) == "HELLO");

    // Silent link (no PING for 6 s): the board gives up and reconnects.
    int silent = conn;
    conn = acceptWithin(listener, 9000);
    sleepMs(100);
    CHECK(conn >= 0 && connects == 3);
    close(silent);

    auto begin = std::chrono::steady_clock::now();
    network::stop();
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(1000));
    CHECK(!network::connected());

    network::send("DROPPED");
    close(conn);
    close(listener);
}

// --- goal_cam.py integration ------------------------------------------------

static void put32(std::vector<uint8_t>& d, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        d.push_back(uint8_t(v >> (8 * i)));
}

static void chunk(std::vector<uint8_t>& d, const char* id, const std::vector<uint8_t>& body)
{
    d.insert(d.end(), id, id + 4);
    put32(d, uint32_t(body.size()));
    d.insert(d.end(), body.begin(), body.end());
    if (body.size() & 1)
        d.push_back(0);
}

static std::vector<uint8_t> list(const char* type, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> d(type, type + 4);
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

static std::vector<uint8_t> fakeJpeg(int n, int extra)
{
    std::vector<uint8_t> j = {0xFF, 0xD8, uint8_t(n)};
    j.insert(j.end(), extra, uint8_t(n));
    j.push_back(0xFF);
    j.push_back(0xD9);
    return j;
}

// An MJPEG AVI laid out like OpenCV's writer output.
static std::vector<uint8_t> makeAvi(int frames, int fps, bool finished)
{
    std::vector<uint8_t> avih;
    put32(avih, 1000000 / fps);
    for (int i = 0; i < 7; ++i)
        put32(avih, i == 3 ? frames : 0);
    put32(avih, 640);
    put32(avih, 480);
    for (int i = 0; i < 4; ++i)
        put32(avih, 0);

    std::vector<uint8_t> strl;
    chunk(strl, "strh", std::vector<uint8_t>(56, 0));
    chunk(strl, "strf", std::vector<uint8_t>(40, 0));

    std::vector<uint8_t> hdrl;
    chunk(hdrl, "avih", avih);
    chunk(hdrl, "LIST", list("strl", strl));

    std::vector<uint8_t> movi;
    chunk(movi, "JUNK", std::vector<uint8_t>(7, 0));
    for (int i = 0; i < frames; ++i)
    {
        chunk(movi, "00dc", fakeJpeg(i, i % 2));
        chunk(movi, "01wb", {1, 2, 3, 4});
    }

    std::vector<uint8_t> body = {'A', 'V', 'I', ' '};
    chunk(body, "LIST", list("hdrl", hdrl));
    chunk(body, "LIST", list("movi", movi));
    chunk(body, "idx1", std::vector<uint8_t>(16 * frames, 0));

    std::vector<uint8_t> riff = {'R', 'I', 'F', 'F'};
    put32(riff, finished ? uint32_t(body.size()) : 0);
    riff.insert(riff.end(), body.begin(), body.end());
    return riff;
}

static void writeFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
}

static std::string tempDir()
{
    const char* base = std::getenv("TMPDIR");
    std::string tmpl = std::string(base ? base : "/tmp") + "/true_goal_test_XXXXXX";
    std::vector<char> path(tmpl.begin(), tmpl.end());
    path.push_back(0);
    return mkdtemp(path.data()) ? std::string(path.data()) : "";
}

static void testAvi()
{
    std::string dir = tempDir();
    CHECK(!dir.empty());

    std::vector<camera::FramePtr> frames;
    int fps = 0;

    writeFile(dir + "/done.avi", makeAvi(5, 15, true));
    CHECK(goalcam::readAvi(dir + "/done.avi", frames, fps));
    CHECK(frames.size() == 5 && fps == 15);
    CHECK(frames[0]->width == 640 && frames[0]->height == 480);
    CHECK(frames[3]->jpeg == fakeJpeg(3, 1) && frames[4]->timeMs == 266);
    CHECK(frames[0]->rgb.empty());

    writeFile(dir + "/writing.avi", makeAvi(5, 15, false));
    CHECK(!goalcam::readAvi(dir + "/writing.avi", frames, fps));

    std::vector<uint8_t> cut = makeAvi(5, 15, true);
    cut.resize(cut.size() - 20);
    writeFile(dir + "/cut.avi", cut);
    CHECK(!goalcam::readAvi(dir + "/cut.avi", frames, fps));

    writeFile(dir + "/junk.avi", {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'});
    CHECK(!goalcam::readAvi(dir + "/junk.avi", frames, fps));
    CHECK(!goalcam::readAvi(dir + "/missing.avi", frames, fps));

    writeFile(dir + "/slow.avi", makeAvi(3, 10, true));
    CHECK(goalcam::readAvi(dir + "/slow.avi", frames, fps) && fps == 10);
}

struct CamEvents {
    std::mutex m;
    std::vector<std::string> status;
    std::vector<std::string> evidence;
    std::vector<std::pair<std::string, int>> clips;
    int clipFps = 0;
    double fps = 0, modelMs = 0;
    bool ball = false;

    std::string lastStatus()
    {
        std::lock_guard<std::mutex> l(m);
        return status.empty() ? "" : status.back();
    }
};

static void appendLine(const std::string& path, const std::string& line)
{
    std::ofstream(path, std::ios::app) << line << "\n";
}

static std::string clipName(long long offset)
{
    time_t t = time(nullptr) + offset;
    char name[64];
    std::strftime(name, sizeof(name), "goal_%Y%m%d_%H%M%S.avi", std::localtime(&t));
    return name;
}

static int freeUdpPort()
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    close(s);
    return ntohs(addr.sin_port);
}

static int bindUdp(int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 ? s : (close(s), -1);
}

// Starts the adapter with "tail -f out.log" standing in for goal_cam.py's
// output, then binds the trigger port the way goal_cam.py would.
static int startStandIn(const std::string& dir, CamEvents& ev, int timeoutMs)
{
    int port = freeUdpPort();

    goalcam::Config config;
    config.dir = dir;
    config.command = "tail -n +1 -f out.log";
    config.triggerPort = port;
    config.clipTimeoutMs = timeoutMs;

    goalcam::Callbacks cb;
    cb.status = [&](const std::string& s) { std::lock_guard<std::mutex> l(ev.m); ev.status.push_back(s); };
    cb.evidence = [&](const std::string& id, const goalcam::Evidence& e, const std::string& r) {
        std::lock_guard<std::mutex> l(ev.m);
        std::string counts = e.measured ? "(" + std::to_string(e.runs) + "/" + std::to_string(e.ballRuns) + ")" : "";
        ev.evidence.push_back(id + " " + e.result + counts + " " + r);
    };
    cb.clip = [&](const std::string& id, std::vector<camera::FramePtr> frames, int fps) {
        std::lock_guard<std::mutex> l(ev.m);
        ev.clips.push_back({id, int(frames.size())});
        ev.clipFps = fps;
    };
    cb.stats = [&](double fps, double ms, bool ball) {
        std::lock_guard<std::mutex> l(ev.m);
        ev.fps = fps;
        ev.modelMs = ms;
        ev.ball = ball;
    };

    goalcam::start(config, cb);
    for (int i = 0; i < 50 && ev.lastStatus() != "starting"; ++i)
        sleepMs(10);
    sleepMs(100);
    return bindUdp(port);
}

static void testGoalCamAdapter()
{
    std::string dir = tempDir();
    std::string log = dir + "/out.log";
    std::string replays = dir + "/replays";
    mkdir(replays.c_str(), 0755);
    writeFile(replays + "/goal_old.avi", makeAvi(4, 15, true));
    appendLine(log, "live view: open  http://1.2.3.4:8080  in the browser on your PC");

    CamEvents ev;
    int udp = startStandIn(dir, ev, 1500);
    CHECK(udp >= 0);

    appendLine(log, "cam 12 fps | model 310 ms, 3.1 runs/s | ball at (320, 240) score 0.91 | viewers 1 | buffer 7.0 s");
    sleepMs(500);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(!ev.status.empty() && ev.status[0] == "starting" && ev.status.back() == "yolo");
        CHECK(ev.fps == 12 && ev.modelMs == 310 && ev.ball);
    }

    // The trigger reaches goal_cam.py's UDP port as "goal".
    CHECK(goalcam::capture("E1", 0));
    char buf[16] = {};
    pollfd p = {udp, POLLIN, 0};
    CHECK(poll(&p, 1, 1000) == 1 && recv(udp, buf, sizeof(buf), 0) == 4 && std::string(buf) == "goal");

    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "GOAL - camera saw the ball: YES (82 frames, 17 ball detections)");
    writeFile(replays + "/goal_new_clean.avi", makeAvi(3, 15, true));
    writeFile(replays + "/goal_new.avi", makeAvi(6, 15, false));
    sleepMs(600);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 1 && ev.evidence[0] == "E1 seen pending");
        CHECK(ev.clips.empty());
    }

    writeFile(replays + "/goal_new.avi", makeAvi(6, 15, true));
    sleepMs(600);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.clips.size() == 1 && ev.clips[0].first == "E1" && ev.clips[0].second == 6);
        CHECK(ev.clipFps == 15);
    }

    // Ignored by goal_cam.py's cooldown: evidence and replay both unavailable.
    goalcam::capture("E2", 0);
    appendLine(log, "goal trigger from udp message ignored (too soon after the last one)");
    sleepMs(2200);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 2 && ev.evidence[1] == "E2 ignored unavailable");
        CHECK(ev.clips.size() == 1);
    }

    // No ball, but the clip never arrives: replay unavailable, evidence kept.
    goalcam::capture("E3", 0);
    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "GOAL - camera saw the ball: NO (40 frames, 0 ball detections)");
    sleepMs(2200);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 4 && ev.evidence[2] == "E3 missed pending" &&
              ev.evidence[3] == "E3 missed unavailable");
    }

    // A YES/NO before goal_cam.py acknowledged this trigger is not this event's.
    goalcam::capture("E4", 0);
    appendLine(log, "GOAL - camera saw the ball: YES (82 frames, 9 ball detections)");
    sleepMs(2200);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 5 && ev.evidence[4] == "E4 unavailable unavailable");
    }

    // Too little video buffered: insufficient, replay unavailable, straight away.
    goalcam::capture("E5", 0);
    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "not enough video in the buffer for a replay");
    sleepMs(500);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 6 && ev.evidence[5] == "E5 insufficient unavailable");
    }

    // EVIDENCE lists the inference runs; it is judged around the beam break
    // (1 s before the trigger here), and the YES that follows does not change it.
    goalcam::capture("E6", 1000);
    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "EVIDENCE -900:0.910:200:240 -600:0.880:330:250 -300:0.000 0:0.000");
    appendLine(log, "GOAL - camera saw the ball: YES (82 frames, 17 ball detections)");
    sleepMs(500);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 7 && ev.evidence[6] == "E6 confident(4/2) pending");
    }

    // A malformed EVIDENCE line falls back to the unverified YES.
    goalcam::capture("E7", 1000);
    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "EVIDENCE nonsense");
    appendLine(log, "GOAL - camera saw the ball: YES (82 frames, 17 ball detections)");
    sleepMs(500);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 8 && ev.evidence[7] == "E7 seen pending");
    }

    // EVIDENCE before goal_cam.py accepted this trigger belongs to an older event.
    goalcam::capture("E8", 0);
    appendLine(log, "EVIDENCE 0:0.95 100:0.95 200:0.95");
    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "GOAL - camera saw the ball: NO (40 frames, 0 ball detections)");
    sleepMs(500);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 9 && ev.evidence[8] == "E8 missed pending");
    }

    appendLine(log, "camera stopped giving frames");
    sleepMs(500);
    CHECK(ev.lastStatus() == "offline");

    auto begin = std::chrono::steady_clock::now();
    goalcam::stop();
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(4));
    close(udp);
}

static void testCloseEvents()
{
    std::string dir = tempDir();
    std::string log = dir + "/out.log";
    std::string replays = dir + "/replays";
    mkdir(replays.c_str(), 0755);
    appendLine(log, "start");

    CamEvents ev;
    int udp = startStandIn(dir, ev, 20000);
    CHECK(udp >= 0);

    // Event A's clip is still being saved when event B is triggered 3 s later.
    goalcam::capture("B", 0);
    writeFile(replays + "/" + clipName(-3), makeAvi(4, 15, true));
    sleepMs(600);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.clips.empty());
    }

    // A's YES arrives before goal_cam.py acknowledges B's trigger.
    appendLine(log, "GOAL - camera saw the ball: YES (80 frames, 20 ball detections)");
    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "GOAL - camera saw the ball: NO (80 frames, 0 ball detections)");
    writeFile(replays + "/" + clipName(0), makeAvi(7, 15, true));
    sleepMs(600);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.clips.size() == 1 && ev.clips[0].first == "B" && ev.clips[0].second == 7);
        CHECK(ev.evidence.size() == 1 && ev.evidence[0] == "B missed pending");
    }

    goalcam::stop();
    close(udp);
}

static void testNoSecondGoalCam()
{
    std::string dir = tempDir();
    appendLine(dir + "/out.log", "should never be read");

    // Something (a goal_cam.py started by hand) already owns the trigger port.
    int port = freeUdpPort();
    int other = bindUdp(port);
    CHECK(other >= 0);

    CamEvents ev;
    goalcam::Config config;
    config.dir = dir;
    config.command = "tail -n +1 -f out.log";
    config.triggerPort = port;

    goalcam::Callbacks cb;
    cb.status = [&](const std::string& s) { std::lock_guard<std::mutex> l(ev.m); ev.status.push_back(s); };
    goalcam::start(config, cb);
    sleepMs(500);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.status.size() == 1 && ev.status[0] == "offline");
    }

    // Once it is gone, true_goal starts its own copy.
    close(other);
    sleepMs(5500);
    CHECK(ev.lastStatus() == "starting");
    goalcam::stop();
}

static void testControllerWithGoalCam()
{
    gpio_mock::reset();
    CHECK(leds::init());
    CHECK(buzzer::init());
    replay::configure(15, 3000, 2000);
    sent.clear();

    std::vector<std::string> captures;
    controller::init(
        [](const std::string& line) { sent.text.push_back(line); },
        [](int slot, const std::string& header, controller::Bytes data) {
            CHECK(data && !data->empty());
            sent.frames.push_back({slot, header});
        },
        8);
    controller::setCaptureHook([&](const std::string& id, int) { captures.push_back(id); return true; });
    controller::setCameraStatus("yolo");

    controller::detected(controller::Source::Ir, 9000, 10000);
    std::string id = controller::eventId();
    CHECK(captures.size() == 1 && captures[0] == id);
    CHECK(sent.count("EVENT " + id + " ir") == 1 && sent.last("CAMERA") == "CAMERA " + id + " pending pending");
    checkLeds(0, 0, 0);

    controller::tick(10000);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 0 0 0 0");
    CHECK(sent.frames.empty());

    // An old goal_cam.py's plain YES is unverified: the referee decides.
    controller::cameraEvidence("other", evidenceOf("seen"), "pending", 10000);
    controller::cameraEvidence(id, evidenceOf("seen"), "pending", 10000);
    CHECK(sent.last("CAMERA") == "CAMERA " + id + " seen pending" && sent.count("CAMERA") == 2);
    CHECK(sent.last("AWAITING") == "AWAITING " + id + " unverified");
    checkLeds(0, 1, 0);

    std::vector<camera::FramePtr> clip;
    for (int i = 0; i < 8; ++i)
    {
        auto f = std::make_shared<camera::Frame>();
        f->width = 640;
        f->height = 480;
        f->jpeg = fakeJpeg(i, 0);
        clip.push_back(f);
    }

    controller::cameraClip("other", clip, 10);
    controller::tick(10010);
    CHECK(sent.frames.empty());

    controller::cameraClip(id, clip, 10);
    CHECK(sent.last("CAMERA") == "CAMERA " + id + " seen ready");
    controller::tick(10020);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 0 8 0 1");
    CHECK(sent.frames.size() == 1 && sent.frames[0].first == controller::ReplaySlot);
    CHECK(sent.frames[0].second == "FRAME REPLAY " + id + " 0 640 480 -1 -1 0 0 5 jpeg");

    // A later report cannot take back a replay that already arrived.
    controller::cameraEvidence(id, evidenceOf("seen"), "unavailable", 10025);
    CHECK(sent.last("CAMERA") == "CAMERA " + id + " seen ready");

    controller::turn(1);
    controller::tick(10030);
    CHECK(sent.frames.size() == 2 && startsWith(sent.frames[1].second, "FRAME REPLAY " + id + " 1 "));

    // Playback runs at the clip's own frame rate (10 fps).
    controller::press();
    controller::tick(20000);
    controller::tick(20050);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 2 8 1 1");
    controller::tick(20100);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 3 8 1 1");
    controller::press();

    sent.clear();
    controller::resync();
    CHECK(sent.count("EVENT " + id) == 1 && sent.last("CAMERA") == "CAMERA " + id + " seen ready");

    controller::message("DECISION " + id + " GOAL", 21000);
    checkLeds(1, 0, 0);
    sent.clear();
    controller::cameraEvidence(id, evidenceOf("missed"), "pending", 21005);
    controller::cameraClip(id, clip, 10);
    controller::tick(21010);
    CHECK(sent.count("CAMERA") == 0 && sent.count("REPLAY") == 0 && sent.frames.empty());

    controller::setCaptureHook(nullptr);
    buzzer::cleanup();
    leds::cleanup();
}

int main()
{
    std::signal(SIGPIPE, SIG_IGN);

    testDecoder();
    testButton();
    testEncoderThread();
    testDetector();
    testReplay();
    testPinsDistinct();
    testLeds();
    testController();
    testHybrid();
    testEvaluate();
    testBuzzerWhistle();
    testBeamCheck();
    testIrThread();
    testCamera();
    testNetwork();
    testAvi();
    testGoalCamAdapter();
    testCloseEvents();
    testNoSecondGoalCam();
    testControllerWithGoalCam();

    if (failures)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}
