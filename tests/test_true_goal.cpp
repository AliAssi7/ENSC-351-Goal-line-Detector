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

static void checkLeds(int green, int red)
{
    CHECK(gpio_mock::value("GPIO5") == green);
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

static void testController()
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

    // Both LEDs stay off at startup and while monitoring.
    checkLeds(0, 0);
    CHECK(controller::state() == controller::State::Monitoring);

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

    // Camera event.
    sent.clear();
    controller::detected(controller::Source::Camera, 990, 1000);
    std::string id = controller::eventId();
    CHECK(!id.empty());
    CHECK(sent.text.size() == 1 && sent.text[0] == "EVENT " + id + " camera");
    checkLeds(0, 0);

    // No live frames during the review; post-event frames join the replay.
    feedFrames(1056, 1056 + 66 * 5);
    CHECK(sent.frames.empty());

    controller::tick(1500);
    CHECK(sent.last("STATUS") == "STATUS review sim offline " + id);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 15 22 0 0");
    CHECK(sent.frames.size() == 1 && sent.frames[0].first == controller::ReplaySlot);
    CHECK(startsWith(sent.frames[0].second, "FRAME REPLAY " + id + " 15 320 240"));

    // IR evidence within the window joins the same event; repeats do not.
    controller::detected(controller::Source::Ir, 900, 1900);
    controller::detected(controller::Source::Ir, 900, 2000);
    controller::detected(controller::Source::Camera, 2000, 2000);
    CHECK(sent.count("EVENT") == 2 && sent.last("EVENT") == "EVENT " + id + " camera,ir");
    CHECK(controller::eventId() == id);

    // Encoder steps move the shared replay and send the new frame.
    sent.clear();
    controller::turn(1);
    controller::tick(1600);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 16 22 0 0");
    CHECK(sent.frames.size() == 1 && startsWith(sent.frames[0].second, "FRAME REPLAY " + id + " 16 "));

    for (int i = 0; i < 30; ++i)
        controller::turn(1);
    controller::tick(1610);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 21 22 0 0");

    // Dashboard commands act on the same replay.
    controller::message("REPLAY SEEK 3", 1620);
    controller::tick(1620);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 3 22 0 0");
    controller::message("REPLAY SEEK 99", 1630);
    controller::message("REPLAY BOGUS", 1630);
    controller::message("REPLAY SEEK", 1630);
    controller::tick(1630);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 3 22 0 0");

    controller::press();
    controller::tick(1700);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 4 22 1 0");
    controller::message("REPLAY PAUSE", 1750);
    controller::tick(1800);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 4 22 0 0");
    controller::message("REPLAY PLAY", 1810);
    controller::tick(1820);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 5 22 1 0");
    controller::message("REPLAY PAUSE", 1830);

    // Reconnecting resends the event, status and replay frame.
    sent.clear();
    controller::resync();
    controller::tick(1840);
    CHECK(sent.count("EVENT " + id) == 1 && sent.count("STATUS") == 1 && sent.count("REPLAY") == 1);
    CHECK(sent.frames.size() == 1);

    // Invalid and stale decisions are ignored.
    controller::message("DECISION wrong-id GOAL", 1900);
    controller::message("DECISION " + id + " MAYBE", 1900);
    controller::message("DECISION", 1900);
    controller::message("garbage", 1900);
    CHECK(controller::state() == controller::State::Review);
    checkLeds(0, 0);

    // Confirmed goal: green for 3 s and the chime (two tones) once.
    controller::message("DECISION " + id + " GOAL", 2000);
    checkLeds(1, 0);
    CHECK(controller::state() == controller::State::Goal);
    CHECK(buzzer::active());
    sleepMs(800);
    CHECK(!buzzer::active() && gpio_mock::value("GPIO25") == 0);

    controller::message("DECISION " + id + " GOAL", 2100);
    controller::message("DECISION " + id + " NO_GOAL", 2100);
    controller::detected(controller::Source::Camera, 2100, 2100);
    sleepMs(50);
    CHECK(gpio_mock::bursts("GPIO25") == 2);
    CHECK(controller::state() == controller::State::Goal);
    checkLeds(1, 0);

    controller::tick(3000);
    controller::tick(4990);
    CHECK(controller::state() == controller::State::Goal);
    checkLeds(1, 0);
    controller::tick(5000);
    CHECK(controller::state() == controller::State::Monitoring);
    checkLeds(0, 0);

    // Manual review rejected: no sound, red for 3 s, then off.
    sent.clear();
    controller::message("REVIEW", 9000);
    std::string manual = controller::eventId();
    CHECK(manual != id && sent.last("EVENT") == "EVENT " + manual + " manual");
    checkLeds(0, 0);
    controller::message("REVIEW", 9010);
    CHECK(controller::eventId() == manual && sent.count("EVENT") == 2);
    controller::message("DECISION " + manual + " NO_GOAL", 9100);
    checkLeds(0, 1);
    CHECK(!buzzer::active());
    controller::tick(12090);
    checkLeds(0, 1);
    controller::tick(12100);
    checkLeds(0, 0);

    // Sensor and camera faults are shown on the dashboard, not the LEDs.
    controller::setIrStatus("error");
    controller::setCameraStatus("offline");
    controller::tick(14200);
    checkLeds(0, 0);
    controller::setIrStatus("ok");
    controller::setCameraStatus("sim");
    controller::tick(14500);
    checkLeds(0, 0);
    CHECK(gpio_mock::bursts("GPIO25") == 2);

    // Late evidence for a closed event opens a new one instead.
    controller::detected(controller::Source::Ir, 9200, 9200);
    std::string late = controller::eventId();
    CHECK(!late.empty() && late != manual);
    controller::message("DECISION " + late + " NO_GOAL", 9300);

    // Several goals in a row, each beeping once.
    std::vector<std::string> ids;
    long long now = 20000;
    for (int i = 0; i < 3; ++i)
    {
        controller::detected(controller::Source::Ir, now - 1000, now);
        ids.push_back(controller::eventId());
        controller::message("DECISION " + ids.back() + " GOAL", now + 100);
        sleepMs(1150);
        controller::tick(now + 200);
        controller::tick(now + 5300);
        CHECK(controller::state() == controller::State::Monitoring);
        now += 10000;
    }
    CHECK(gpio_mock::bursts("GPIO25") == 8);
    CHECK(ids[0] != ids[1] && ids[1] != ids[2]);
    checkLeds(0, 0);

    // A review can start while the goal LED is still lit: the LED goes off.
    controller::detected(controller::Source::Camera, now, now);
    controller::message("DECISION " + controller::eventId() + " GOAL", now);
    controller::message("REVIEW", now + 100);
    checkLeds(0, 0);

    // Rejected, then a quick new review confirmed: the new LED gets its own 3 s.
    controller::message("DECISION " + controller::eventId() + " NO_GOAL", now + 200);
    checkLeds(0, 1);
    controller::message("REVIEW", now + 500);
    checkLeds(0, 0);
    controller::message("DECISION " + controller::eventId() + " GOAL", now + 1000);
    checkLeds(1, 0);
    controller::tick(now + 3500);
    checkLeds(1, 0);
    controller::tick(now + 4000);
    checkLeds(0, 0);
    sleepMs(800);
    now += 10000;

    // Status heartbeat.
    sent.clear();
    controller::tick(now + 200);
    controller::tick(now + 300);
    controller::tick(now + 1300);
    CHECK(sent.count("STATUS") == 2);

    buzzer::cleanup();
    leds::cleanup();
    CHECK(!gpio_mock::isOpen("GPIO25") && !gpio_mock::isOpen("GPIO5"));
    CHECK(gpio_mock::value("GPIO26") == 0);

    buzzer::cleanup();
    leds::cleanup();
    buzzer::chime();
    leds::show(leds::Color::Red);
    controller::detected(controller::Source::Camera, now, now);
}

static void waitForBuzzer()
{
    sleepMs(20);
    for (int i = 0; i < 300 && buzzer::active(); ++i)
        sleepMs(5);
    sleepMs(50);   // a gap, so the mock counts the next tone as a new burst
}

static void checkTone(const gpio_mock::Burst& b, int hz, int ms)
{
    double lengthMs = (b.endUs - b.startUs) / 1000.0;
    double duty = 100.0 * b.highUs / (b.cycles * 1e6 / hz);
    std::cout << "Buzzer tone: " << b.cycles << " cycles in " << int(lengthMs) << " ms ("
              << int(b.cycles * 1000 / std::max(1.0, lengthMs)) << " Hz), duty " << int(duty + 0.5) << "%\n";

    CHECK(lengthMs >= ms - 10 && lengthMs <= ms + 2);
    CHECK(b.cycles >= hz * ms / 1000 * 95 / 100 && b.cycles <= hz * ms / 1000 + 1);
    CHECK(duty >= 12 && duty <= 22);
}

static void testBuzzerChime()
{
    using std::chrono::steady_clock;
    gpio_mock::reset();

    // Silent at startup and while idle.
    CHECK(buzzer::init());
    sleepMs(200);
    CHECK(gpio_mock::value("GPIO25") == 0 && gpio_mock::rises("GPIO25") == 0 && !buzzer::active());

    // chime() returns at once: 1200 Hz for 150 ms, 80 ms silence, 1600 Hz for 200 ms.
    auto start = steady_clock::now();
    buzzer::chime();
    CHECK(steady_clock::now() - start < std::chrono::milliseconds(5));
    CHECK(buzzer::active());
    waitForBuzzer();
    CHECK(steady_clock::now() - start < std::chrono::milliseconds(600));

    std::vector<gpio_mock::Burst> log = gpio_mock::burstLog("GPIO25");
    CHECK(log.size() == 2);
    if (log.size() == 2)
    {
        checkTone(log[0], 1200, 150);
        checkTone(log[1], 1600, 200);
        long long gapMs = (log[1].startUs - log[0].endUs) / 1000;
        CHECK(gapMs >= 75 && gapMs <= 95);
    }
    CHECK(gpio_mock::value("GPIO25") == 0 && !buzzer::active());

    // Repeated requests while it plays neither overlap nor restart it.
    buzzer::chime();
    sleepMs(100);
    buzzer::chime();
    sleepMs(200);
    buzzer::chime();
    waitForBuzzer();
    log = gpio_mock::burstLog("GPIO25");
    CHECK(log.size() == 4);
    if (log.size() == 4)
        CHECK(log[3].endUs - log[2].startUs < 450000);

    // A later, separate goal sounds again.
    buzzer::chime();
    waitForBuzzer();
    CHECK(gpio_mock::burstLog("GPIO25").size() == 6);

    // Shutdown during a tone, or during the silence, stops at once with the pin low.
    buzzer::chime();
    sleepMs(50);
    start = steady_clock::now();
    buzzer::cleanup();
    CHECK(steady_clock::now() - start < std::chrono::milliseconds(20));
    CHECK(gpio_mock::value("GPIO25") == 0 && !gpio_mock::isOpen("GPIO25"));
    CHECK(gpio_mock::burstLog("GPIO25").size() == 7);

    gpio_mock::reset();
    CHECK(buzzer::init());
    buzzer::chime();
    sleepMs(185);
    start = steady_clock::now();
    buzzer::cleanup();
    CHECK(steady_clock::now() - start < std::chrono::milliseconds(20));
    CHECK(gpio_mock::burstLog("GPIO25").size() == 1 && gpio_mock::value("GPIO25") == 0);

    // No sound after shutdown or before init.
    buzzer::chime();
    CHECK(!buzzer::active());
    buzzer::cleanup();

    // An unavailable buzzer line is reported, not hidden.
    gpio_mock::setBroken("GPIO25", true);
    CHECK(!buzzer::init());
    buzzer::chime();
    CHECK(!buzzer::active());
    gpio_mock::reset();
}

static void testLeds()
{
    gpio_mock::reset();
    CHECK(leds::init());
    checkLeds(0, 0);

    CHECK(leds::show(leds::Color::Green));
    checkLeds(1, 0);
    CHECK(leds::show(leds::Color::Red));
    checkLeds(0, 1);
    CHECK(leds::show(leds::Color::Off));
    checkLeds(0, 0);

    // A write failure is reported.
    gpio_mock::setBroken("GPIO5", true);
    CHECK(!leds::show(leds::Color::Green));
    gpio_mock::setBroken("GPIO5", false);
    leds::cleanup();
    checkLeds(0, 0);
    CHECK(!gpio_mock::isOpen("GPIO5") && !gpio_mock::isOpen("GPIO26"));

    // One missing LED does not disable the other.
    gpio_mock::reset();
    gpio_mock::setBroken("GPIO26", true);
    CHECK(!leds::init());
    CHECK(leds::show(leds::Color::Green));
    CHECK(gpio_mock::value("GPIO5") == 1);
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
                                    pins::kLedGreen, pins::kLedRed, pins::kBuzzer};
    all.insert(all.end(), std::begin(pins::kIrBeams), std::end(pins::kIrBeams));
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
    cb.evidence = [&](const std::string& id, const std::string& e, const std::string& r) {
        std::lock_guard<std::mutex> l(ev.m);
        ev.evidence.push_back(id + " " + e + " " + r);
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
    CHECK(goalcam::capture("E1"));
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
    goalcam::capture("E2");
    appendLine(log, "goal trigger from udp message ignored (too soon after the last one)");
    sleepMs(2200);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 2 && ev.evidence[1] == "E2 ignored unavailable");
        CHECK(ev.clips.size() == 1);
    }

    // No ball, but the clip never arrives: replay unavailable, evidence kept.
    goalcam::capture("E3");
    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "GOAL - camera saw the ball: NO (40 frames, 0 ball detections)");
    sleepMs(2200);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 4 && ev.evidence[2] == "E3 missed pending" &&
              ev.evidence[3] == "E3 missed unavailable");
    }

    // A YES/NO before goal_cam.py acknowledged this trigger is not this event's.
    goalcam::capture("E4");
    appendLine(log, "GOAL - camera saw the ball: YES (82 frames, 9 ball detections)");
    sleepMs(2200);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 5 && ev.evidence[4] == "E4 unavailable unavailable");
    }

    // Too little video buffered: both unavailable straight away.
    goalcam::capture("E5");
    appendLine(log, "GOAL trigger from udp message, recording 1.5 more seconds");
    appendLine(log, "not enough video in the buffer for a replay");
    sleepMs(500);
    {
        std::lock_guard<std::mutex> l(ev.m);
        CHECK(ev.evidence.size() == 6 && ev.evidence[5] == "E5 unavailable unavailable");
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
    goalcam::capture("B");
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
    controller::setCaptureHook([&](const std::string& id) { captures.push_back(id); });
    controller::setCameraStatus("yolo");

    controller::detected(controller::Source::Ir, 9000, 10000);
    std::string id = controller::eventId();
    CHECK(captures.size() == 1 && captures[0] == id);
    CHECK(sent.count("EVENT " + id + " ir") == 1 && sent.last("CAMERA") == "CAMERA " + id + " pending pending");
    checkLeds(0, 0);

    controller::tick(10000);
    CHECK(sent.last("REPLAY") == "REPLAY " + id + " 0 0 0 0");
    CHECK(sent.frames.empty());

    controller::cameraEvidence("other", "seen", "pending");
    controller::cameraEvidence(id, "seen", "pending");
    CHECK(sent.last("CAMERA") == "CAMERA " + id + " seen pending" && sent.count("CAMERA") == 2);

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
    controller::cameraEvidence(id, "seen", "unavailable");
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
    checkLeds(1, 0);
    sent.clear();
    controller::cameraEvidence(id, "missed", "pending");
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
    testBuzzerChime();
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
