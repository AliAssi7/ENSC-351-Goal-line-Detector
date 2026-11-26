#include "buzzer.h"
#include "camera.h"
#include "controller.h"
#include "detector.h"
#include "encoder.h"
#include "goalcam.h"
#include "leds.h"
#include "network.h"
#include "pins.h"
#include "replay.h"
#include "sensors.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using controller::Source;

static std::atomic<bool> running{true};

static std::mutex detectorMutex;
static detector::Detector goalDetector;
static std::string lineFile = "goal_line.txt";
static int frameWidth = 320;
static int frameHeight = 240;

static void onSignal(int)
{
    running = false;
}

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool parseLine(const std::string& text, detector::Line& l)
{
    std::string s = text;
    for (char& c : s)
        if (c == ',')
            c = ' ';

    std::istringstream in(s);
    return bool(in >> l.x1 >> l.y1 >> l.x2 >> l.y2 >> l.gx >> l.gy);
}

static std::string lineMessage()
{
    std::lock_guard<std::mutex> lock(detectorMutex);
    const detector::Line& l = goalDetector.line();

    return "LINE " + std::to_string(l.x1) + " " + std::to_string(l.y1) + " " +
           std::to_string(l.x2) + " " + std::to_string(l.y2) + " " +
           std::to_string(l.gx) + " " + std::to_string(l.gy);
}

static void onServerLine(const std::string& line)
{
    if (line.rfind("LINE ", 0) != 0)
    {
        controller::message(line, nowMs());
        return;
    }

    detector::Line l;

    if (parseLine(line.substr(5), l))
    {
        std::lock_guard<std::mutex> lock(detectorMutex);

        if (detector::valid(l, frameWidth, frameHeight))
        {
            goalDetector.setLine(l);
            std::ofstream(lineFile) << line.substr(5) << "\n";
            std::cout << "Goal line calibrated:" << line.substr(4) << "\n";
        }
        else
        {
            std::cerr << "Rejected goal line calibration\n";
        }
    }

    network::send(lineMessage());
}

static void onFrame(std::shared_ptr<camera::Frame> frame)
{
    detector::Result result;
    bool lineChanged;

    {
        std::lock_guard<std::mutex> lock(detectorMutex);
        frameWidth = frame->width;
        frameHeight = frame->height;

        detector::Line before = goalDetector.line();
        result = goalDetector.process(*frame);
        lineChanged = std::memcmp(&before, &goalDetector.line(), sizeof(before)) != 0;
    }

    if (lineChanged)
        network::send(lineMessage());

    frame->ball = result.found;
    frame->ballX = result.x;
    frame->ballY = result.y;
    frame->ballR = result.radius;
    frame->zone = result.zone;

    camera::FramePtr shared = frame;
    replay::push(shared);

    if (result.crossing)
    {
        std::cout << "Camera: ball wholly over the line\n";
        controller::detected(Source::Camera, frame->timeMs, frame->timeMs);
    }

    controller::cameraFrame(shared);
}

// Only one true_goal may own the GPIO lines, the camera and the server link.
static bool lockInstance()
{
    const char* path = "/tmp/true_goal.lock";
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);

    if (fd < 0)
    {
        std::cerr << "Warning: cannot open " << path << ": " << std::strerror(errno) << "\n";
        return true;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        char pid[16] = {};
        if (read(fd, pid, sizeof(pid) - 1) < 0) {}
        std::cerr << "True Goal is already running (PID " << std::atoi(pid) << ").\n"
                  << "Stop it first with Ctrl+C in its terminal, or: kill " << std::atoi(pid) << "\n";
        close(fd);
        return false;
    }

    std::string text = std::to_string(getpid()) + "\n";
    if (ftruncate(fd, 0) != 0 || write(fd, text.data(), text.size()) < 0) {}
    return true;   // fd stays open, holding the lock until exit
}

static void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static int testLeds()
{
    bool ok = leds::init();

    std::cout << "Green LED (" << pins::kLedGreen << ") on for 2 s\n";
    ok &= leds::show(leds::Color::Green);
    sleepMs(2000);

    std::cout << "Red LED (" << pins::kLedRed << ") on for 2 s\n";
    ok &= leds::show(leds::Color::Red);
    sleepMs(2000);

    ok &= leds::show(leds::Color::Off);
    leds::cleanup();
    std::cout << "Both LEDs off\n";
    return ok ? 0 : 1;
}

static int testBuzzer()
{
    if (!buzzer::init())
    {
        std::cerr << "Buzzer unavailable\n";
        return 1;
    }

    std::cout << "Playing the goal chime once\n";
    buzzer::chime();

    while (buzzer::active())
        sleepMs(10);

    buzzer::cleanup();
    std::cout << "Buzzer off\n";
    return 0;
}

static int testEncoder()
{
    bool started = encoder::start(
        [](int step) { std::cout << (step > 0 ? "CLOCKWISE" : "COUNTERCLOCKWISE") << std::endl; },
        [] { std::cout << "BUTTON PRESSED" << std::endl; });

    if (!started)
    {
        std::cerr << "Rotary encoder unavailable\n";
        return 1;
    }

    std::cout << "Turn or press the encoder. Ctrl+C to stop.\n";

    while (running)
        sleepMs(50);

    encoder::stop();
    std::cout << "Encoder released\n";
    return 0;
}

static void usage()
{
    std::cout <<
        "Usage: true_goal --server HOST [options]\n"
        "  --server HOST         computer running server.js (required)\n"
        "  --port N              board port on the server (default 4000)\n"
        "  --no-ir               do not run the IR sensors\n"
        "  --skip-ir 18[,23]     do not use these IR sensors (GPIO numbers)\n"
        "\n"
        "Camera (default: the YOLO camera program goal_cam.py):\n"
        "  --goalcam-dir DIR     folder with goal_cam.py (default /root/ball)\n"
        "  --goalcam-cmd CMD     command run there (default \"python3 -u goal_cam.py\")\n"
        "  --goalcam-port N      its UDP trigger port (default 5005)\n"
        "  --stream-port N       its MJPEG web port shown on the dashboard (default 8080)\n"
        "  --no-camera           run without any camera\n"
        "  --check-avi FILE      check that a goal_cam.py replay clip can be read, then exit\n"
        "  --test-leds           light green, then red, for 2 s each and exit\n"
        "  --test-buzzer         play the goal chime once and exit\n"
        "  --test-encoder        print encoder turns and presses until Ctrl+C\n"
        "\n"
        "Built-in test camera instead of goal_cam.py (never use both on one device):\n"
        "  --sim                 simulated pitch and ball\n"
        "  --video FILE          video file (needs ffmpeg)\n"
        "  --camera DEVICE       V4L2 camera with YUYV/UYVY\n"
        "  --width N --height N  frame size (default 320x240)\n"
        "  --fps N               capture rate (default 15)\n"
        "  --live-fps N          live preview rate sent to the dashboard (default 8)\n"
        "  --line x1,y1,x2,y2,gx,gy  goal line and a point inside the goal\n"
        "  --line-file FILE      saved calibration (default goal_line.txt)\n"
        "  --ball-hue H          ball hue 0-359 (default 20, orange)\n"
        "  --hue-tolerance N     default 15\n"
        "  --min-saturation N    0-255, default 110\n"
        "  --min-value N         0-255, default 70\n";
}

int main(int argc, char* argv[])
{
#ifdef TRUE_GOAL_MOCK_GPIO
    std::cerr << "*** Mock GPIO build: no real LEDs, buzzer, encoder or IR sensors are used ***\n";
#endif

    std::string host;
    int port = 4000;
    int liveFps = 8;
    int streamPort = 8080;
    bool useIr = true;
    bool builtInCamera = false;
    bool noCamera = false;
    goalcam::Config goalCam;
    camera::Config cam;
    detector::BallColor color;
    std::string lineArg;
    std::string hardwareTest;
    std::vector<std::string> skipIr;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        bool hasValue = i + 1 < argc;
        auto value = [&]() { return std::string(argv[++i]); };
        auto number = [&]() { return std::atoi(argv[++i]); };

        if (arg == "--test-leds" || arg == "--test-buzzer" || arg == "--test-encoder")
        {
            hardwareTest = arg;
        }
        else if (arg == "--skip-ir" && hasValue)
        {
            std::istringstream list(value());
            std::string pin;
            while (std::getline(list, pin, ','))
                skipIr.push_back(pin.rfind("GPIO", 0) == 0 ? pin : "GPIO" + pin);
        }
        else if (arg == "--check-avi" && hasValue)
        {
            std::vector<camera::FramePtr> frames;
            int fps = 0;
            bool ok = goalcam::readAvi(value(), frames, fps);

            std::cout << (ok ? "Readable: " : "Not readable (unfinished or not MJPEG): ")
                      << frames.size() << " frames, " << fps << " fps";
            if (ok)
                std::cout << ", " << frames[0]->width << "x" << frames[0]->height;
            std::cout << "\n";
            return ok ? 0 : 1;
        }
        else if (arg == "--sim")                         cam.simulate = builtInCamera = true;
        else if (arg == "--no-ir")                       useIr = false;
        else if (arg == "--no-camera")                   noCamera = true;
        else if (arg == "--goalcam-dir" && hasValue)     goalCam.dir = value();
        else if (arg == "--goalcam-cmd" && hasValue)     goalCam.command = value();
        else if (arg == "--goalcam-port" && hasValue)    goalCam.triggerPort = number();
        else if (arg == "--stream-port" && hasValue)     streamPort = number();
        else if (arg == "--server" && hasValue)          host = value();
        else if (arg == "--port" && hasValue)            port = number();
        else if (arg == "--camera" && hasValue)          cam.device = value(), builtInCamera = true;
        else if (arg == "--video" && hasValue)           cam.videoFile = value(), builtInCamera = true;
        else if (arg == "--width" && hasValue)           cam.width = number();
        else if (arg == "--height" && hasValue)          cam.height = number();
        else if (arg == "--fps" && hasValue)             cam.fps = number();
        else if (arg == "--live-fps" && hasValue)        liveFps = number();
        else if (arg == "--line" && hasValue)            lineArg = value();
        else if (arg == "--line-file" && hasValue)       lineFile = value();
        else if (arg == "--ball-hue" && hasValue)        color.hue = number();
        else if (arg == "--hue-tolerance" && hasValue)   color.hueTolerance = number();
        else if (arg == "--min-saturation" && hasValue)  color.minSaturation = number();
        else if (arg == "--min-value" && hasValue)       color.minValue = number();
        else
        {
            usage();
            return arg == "--help" ? 0 : 1;
        }
    }

    if (cam.width < 16 || cam.width > 1920 || cam.width % 2 || cam.height < 16 ||
        cam.height > 1080 || cam.fps < 1 || cam.fps > 60 || liveFps < 1 ||
        port <= 0 || port > 65535 || goalCam.triggerPort <= 0 || goalCam.triggerPort > 65535 ||
        streamPort <= 0 || streamPort > 65535)
    {
        std::cerr << "Invalid size, frame rate or port\n";
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    if (!hardwareTest.empty())
    {
        if (!lockInstance())
            return 1;
        if (hardwareTest == "--test-leds")
            return testLeds();
        if (hardwareTest == "--test-buzzer")
            return testBuzzer();
        return testEncoder();
    }

    if (host.empty())
    {
        usage();
        return 1;
    }

    if (!lockInstance())
        return 1;

    bool useGoalCam = !builtInCamera && !noCamera;

    frameWidth = cam.width;
    frameHeight = cam.height;

    detector::Line line = detector::defaultLine(cam.width, cam.height);
    std::string saved;

    if (!lineArg.empty())
    {
        if (!parseLine(lineArg, line) || !detector::valid(line, cam.width, cam.height))
        {
            std::cerr << "Invalid --line\n";
            return 1;
        }
    }
    else if (std::getline(std::ifstream(lineFile), saved))
    {
        detector::Line l;
        if (parseLine(saved, l) && detector::valid(l, cam.width, cam.height))
            line = l;
    }

    goalDetector.setLine(line);
    goalDetector.setColor(color);

    if (!leds::init())
        std::cerr << "Status LEDs not fully available\n";

    if (!buzzer::init())
        std::cerr << "Buzzer unavailable\n";

    replay::configure(cam.fps, 3000, 2000);
    controller::init(network::send, network::sendFrame, liveFps);
    controller::setIrStatus(useIr ? "offline" : "disabled");
    if (noCamera)
        controller::setCameraStatus("disabled");

    network::start(host, port, onServerLine, [useGoalCam, builtInCamera, streamPort] {
        if (builtInCamera)
            network::send(lineMessage());
        // Our own address, since the server may only see a NAT address.
        network::send(useGoalCam ? "STREAM " + std::to_string(streamPort) + " " +
                                       network::localAddress()
                                 : "STREAM -");
        controller::resync();
    });

    if (!encoder::start(controller::turn, controller::press))
        std::cerr << "Rotary encoder unavailable\n";

    if (useGoalCam)
    {
        goalcam::Callbacks callbacks;
        callbacks.status = controller::setCameraStatus;
        callbacks.evidence = controller::cameraEvidence;
        callbacks.clip = controller::cameraClip;
        callbacks.stats = [](double fps, double ms, bool ball) {
            network::send("CAMSTAT " + std::to_string(int(fps + 0.5)) + " " +
                          std::to_string(int(ms + 0.5)) + " " + (ball ? "1" : "0"));
        };

        controller::setCaptureHook([](const std::string& id) {
            if (!goalcam::capture(id))
                std::cerr << "Could not send the replay trigger to goal_cam.py\n";
        });
        goalcam::start(goalCam, callbacks);
    }
    else if (builtInCamera)
    {
        camera::start(cam, onFrame, controller::setCameraStatus);
    }

    if (useIr)
    {
        sensors::start(skipIr,
            [](long long brokenMs, long long now) { controller::detected(Source::Ir, brokenMs, now); },
            controller::setIrStatus);
    }

    std::cout << "True Goal running, server " << host << ":" << port << "\n";

    // Also paces replay playback.
    while (running)
    {
        controller::tick(nowMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Shutting down\n";

    sensors::stop();
    goalcam::stop();
    camera::stop();
    encoder::stop();
    network::stop();
    buzzer::cleanup();
    leds::cleanup();

    return 0;
}
