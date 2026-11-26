#include "goalcam.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace goalcam {

// Automatic-goal thresholds: initial tuning values, not validated accuracy.
// The model runs about 3 times a second on the board, so a ball rolling past
// is only in a few runs; the window includes footage from before the trigger.
static const double kAutoConfidence = 0.75;
static const int kMinBallRuns = 2;          // distinct runs with a confident ball
static const int kMinRuns = 3;              // runs needed in the window to judge at all
static const int kMinBallMovePx = 40;       // a ball that crossed the line moves; a parked one does not
static const int kWindowBeforeMs = 1500;    // window around the beam break
static const int kWindowAfterMs = 1000;

static Config config;
static Callbacks callbacks;
static std::thread worker;
static std::atomic<bool> running{false};

// The one event waiting for goal_cam.py's answer and replay clip.
struct Pending {
    std::string id;
    long long since = 0;
    time_t sentAt = 0;
    int breakOffsetMs = 0;
    Evidence details;
    bool acknowledged = false;
    bool accepted = false;
    bool clipDone = true;
    std::string evidence;
    std::string replay;
    std::set<std::string> known;
};

static std::mutex mutex;
static Pending pending;

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint32_t le32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

static void walk(const std::vector<uint8_t>& d, size_t from, size_t to, int depth,
                 std::vector<camera::FramePtr>& frames, uint32_t& usPerFrame,
                 int& width, int& height)
{
    size_t at = from;

    while (depth < 4 && at + 8 <= to)
    {
        const uint8_t* id = &d[at];
        size_t size = le32(&d[at + 4]);
        size_t body = at + 8;

        if (body + size > to)
            break;

        if (std::memcmp(id, "LIST", 4) == 0 && size >= 4)
        {
            walk(d, body + 4, body + size, depth + 1, frames, usPerFrame, width, height);
        }
        else if (std::memcmp(id, "avih", 4) == 0 && size >= 40)
        {
            usPerFrame = le32(&d[body]);
            width = int(le32(&d[body + 32]));
            height = int(le32(&d[body + 36]));
        }
        else if ((id[2] == 'd' && (id[3] == 'c' || id[3] == 'b')) && size >= 4 &&
                 d[body] == 0xFF && d[body + 1] == 0xD8)
        {
            auto frame = std::make_shared<camera::Frame>();
            frame->width = width;
            frame->height = height;
            frame->timeMs = (long long)frames.size() * usPerFrame / 1000;
            frame->jpeg.assign(d.begin() + body, d.begin() + body + size);
            frames.push_back(frame);
        }

        at = body + size + (size & 1);
    }
}

// The RIFF size is only filled in when the writer closes the file; until
// then it is 0 (OpenCV) or 0xFFFFFFFF (FFmpeg).
static bool finished(const uint8_t* header, uint64_t fileSize)
{
    uint64_t riff = uint64_t(le32(header + 4)) + 8;

    return std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header + 8, "AVI ", 4) == 0 &&
           riff <= fileSize && riff + 1 >= fileSize;
}

bool readAvi(const std::string& path, std::vector<camera::FramePtr>& frames, int& fps)
{
    struct stat st;
    uint8_t header[12];
    std::ifstream file(path, std::ios::binary);

    if (stat(path.c_str(), &st) != 0 || !file.read(reinterpret_cast<char*>(header), 12) ||
        !finished(header, uint64_t(st.st_size)))
        return false;

    file.seekg(0);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (d.size() < 12 || !finished(d.data(), d.size()))
        return false;

    uint32_t usPerFrame = 0;
    int width = 0, height = 0;
    frames.clear();
    walk(d, 12, size_t(le32(&d[4])) + 8, 0, frames, usPerFrame, width, height);

    fps = usPerFrame ? int(std::lround(1e6 / usPerFrame)) : 15;
    return !frames.empty();
}

static std::set<std::string> listClips()
{
    std::set<std::string> names;
    DIR* dir = opendir((config.dir + "/replays").c_str());

    if (!dir)
        return names;

    while (dirent* entry = readdir(dir))
    {
        std::string name = entry->d_name;
        bool avi = name.size() > 4 && name.compare(name.size() - 4, 4, ".avi") == 0;
        bool clean = name.size() > 10 && name.compare(name.size() - 10, 10, "_clean.avi") == 0;

        if (avi && !clean)
            names.insert(name);
    }

    closedir(dir);
    return names;
}

// goal_cam.py names clips goal_YYYYmmdd_HHMMSS.avi after the trigger time,
// using the same board clock as true_goal.
static bool clipTime(const std::string& name, time_t& t)
{
    std::tm tm{};
    int n = 0;

    if (std::sscanf(name.c_str(), "goal_%4d%2d%2d_%2d%2d%2d.avi%n", &tm.tm_year, &tm.tm_mon,
                    &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &n) != 6 ||
        n != int(name.size()))
        return false;

    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    t = std::mktime(&tm);
    return t != -1;
}

static void report(const std::string& id, const Evidence& evidence, const std::string& replay)
{
    if (callbacks.evidence)
        callbacks.evidence(id, evidence, replay);
}

Evidence evaluate(const std::vector<Inference>& runs, int breakOffsetMs)
{
    Evidence e;
    std::set<int> counted;
    bool anyBall = false;
    std::vector<const Inference*> confident;

    e.measured = true;

    for (const Inference& run : runs)
    {
        int t = run.dtMs + breakOffsetMs;   // relative to the beam break

        // The same run listed twice is only counted once.
        if (!counted.insert(run.dtMs).second || t < -kWindowBeforeMs || t > kWindowAfterMs)
            continue;

        ++e.runs;
        e.best = std::max(e.best, run.confidence);

        if (run.confidence > 0)
        {
            e.firstMs = anyBall ? std::min(e.firstMs, t) : t;
            e.lastMs = anyBall ? std::max(e.lastMs, t) : t;
            anyBall = true;
        }

        if (run.confidence >= kAutoConfidence)
        {
            ++e.ballRuns;
            if (run.hasPosition)
                confident.push_back(&run);
        }
    }

    // Largest distance between two confident detections; no position, no movement.
    for (const Inference* a : confident)
        for (const Inference* b : confident)
            e.movePx = std::max(e.movePx, int(std::hypot(a->x - b->x, a->y - b->y)));

    if (e.runs < kMinRuns)
        e.result = "insufficient";
    else if (e.ballRuns >= kMinBallRuns)
        e.result = e.movePx >= kMinBallMovePx ? "confident" : "stationary";
    else
        e.result = anyBall ? "low" : "none";

    return e;
}

bool parseEvidence(const std::string& line, std::vector<Inference>& runs)
{
    std::istringstream in(line);
    std::string word;
    runs.clear();

    if (!(in >> word) || word != "EVIDENCE")
        return false;

    while (in >> word)
    {
        Inference run;
        int used = 0;
        int fields = std::sscanf(word.c_str(), "%d:%lf%n:%d:%d%n", &run.dtMs, &run.confidence,
                                 &used, &run.x, &run.y, &used);

        if ((fields != 2 && fields != 4) || used != int(word.size()) ||
            !(run.confidence >= 0 && run.confidence <= 1))
            return false;

        run.hasPosition = fields == 4;
        runs.push_back(run);
    }

    return true;
}

bool capture(const std::string& eventId, int breakOffsetMs)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = Pending();
        pending.id = eventId;
        pending.breakOffsetMs = breakOffsetMs;
        pending.since = nowMs();
        pending.sentAt = time(nullptr);
        pending.clipDone = false;
        pending.evidence = "pending";
        pending.replay = "pending";
        pending.known = listClips();
    }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.triggerPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool sent = sendto(s, "goal", 4, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 4;
    close(s);
    return sent;
}

// goal_cam.py's output is for people, so anything unrecognised is ignored.
// EVIDENCE or YES/NO only count after goal_cam.py has accepted this event's
// trigger, so a late line from the previous replay is never attributed to it.
// EVIDENCE is printed before YES/NO, and the first answer wins.
static void parseLine(const std::string& line)
{
    auto has = [&](const char* text) { return line.find(text) != std::string::npos; };

    double fps, ms;
    if (std::sscanf(line.c_str(), "cam %lf fps | model %lf ms", &fps, &ms) == 2)
    {
        if (callbacks.stats)
            callbacks.stats(fps, ms, has("| ball at"));
        if (callbacks.status)
            callbacks.status("yolo");
    }

    if (has("cant open camera") || has("camera stopped giving frames"))
    {
        if (callbacks.status)
            callbacks.status("offline");

        static bool hinted = false;
        if (!hinted)
            std::cerr << "Camera not delivering frames. Check the USB webcam and that nothing "
                         "else uses it: ls /dev/video*  and  fuser -v /dev/video*\n";
        hinted = true;
    }

    std::string id, replay;
    Evidence evidence;

    {
        std::lock_guard<std::mutex> lock(mutex);
        Pending& p = pending;
        std::string before = p.evidence + p.replay;

        if (p.id.empty())
            return;

        if (!p.acknowledged && has("from udp message"))
        {
            p.acknowledged = true;

            if (has("ignored"))
            {
                p.evidence = "ignored";
                p.replay = "unavailable";
                p.clipDone = true;
            }
            else
            {
                p.accepted = has("GOAL trigger");
            }
        }
        else if (p.accepted && p.evidence == "pending" && line.rfind("EVIDENCE", 0) == 0)
        {
            std::vector<Inference> runs;

            if (parseEvidence(line, runs))
            {
                p.details = evaluate(runs, p.breakOffsetMs);
                p.evidence = p.details.result;
            }
            else
            {
                std::cerr << "Ignoring malformed EVIDENCE line from goal_cam.py\n";
            }
        }
        else if (p.accepted && p.evidence == "pending" && has("camera saw the ball: YES"))
        {
            p.evidence = "seen";
        }
        else if (p.accepted && p.evidence == "pending" && has("camera saw the ball: NO"))
        {
            p.evidence = "missed";
        }
        else if (p.accepted && !p.clipDone && has("not enough video"))
        {
            p.evidence = "insufficient";
            p.replay = "unavailable";
            p.clipDone = true;
        }

        if (p.evidence + p.replay == before)
            return;

        id = p.id;
        evidence = p.details;
        evidence.result = p.evidence;
        replay = p.replay;
    }

    report(id, evidence, replay);
}

static void checkClip()
{
    std::string id;
    Evidence evidence;
    bool timedOut = false;
    std::set<std::string> known;
    time_t sentAt;
    bool accepted;

    {
        std::lock_guard<std::mutex> lock(mutex);
        Pending& p = pending;

        if (p.id.empty() || p.clipDone)
            return;

        if (nowMs() - p.since > config.clipTimeoutMs)
        {
            p.clipDone = true;
            p.replay = "unavailable";
            if (p.evidence == "pending")
                p.evidence = "unavailable";
            evidence = p.details;
            evidence.result = p.evidence;
            timedOut = true;
        }

        id = p.id;
        known = p.known;
        sentAt = p.sentAt;
        accepted = p.accepted;
    }

    if (timedOut)
    {
        std::cerr << "No goal_cam replay for event " << id << "\n";
        report(id, evidence, "unavailable");
        return;
    }

    for (const std::string& name : listClips())
    {
        time_t t;

        if (known.count(name))
            continue;

        // Unrecognised names are only trusted once this trigger was accepted.
        if (clipTime(name, t) ? (t < sentAt - 1 || t > sentAt + 2) : !accepted)
            continue;

        std::vector<camera::FramePtr> frames;
        int fps = 0;

        if (!readAvi(config.dir + "/replays/" + name, frames, fps))
            continue;

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pending.id != id || pending.clipDone)
                return;
            pending.clipDone = true;
        }

        std::cout << "goal_cam replay " << name << ": " << frames.size()
                  << " frames at " << fps << " fps for event " << id << "\n";

        if (callbacks.clip)
            callbacks.clip(id, std::move(frames), fps);
        return;
    }
}

// Another goal_cam.py (for example one started by hand over SSH) already
// owns the trigger port and the camera, so a second copy must not start.
static bool triggerPortInUse()
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.triggerPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool inUse = bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 && errno == EADDRINUSE;
    close(s);
    return inUse;
}

static pid_t spawn(int& out)
{
    // Prepared before fork: only async-signal-safe calls are allowed in the child.
    std::string shell = "export PYTHONUNBUFFERED=1; exec " + config.command;
    pid_t parent = getpid();
    long maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 4096)
        maxFd = 4096;

    int fds[2];
    if (pipe(fds) < 0)
        return -1;

    pid_t pid = fork();

    if (pid == 0)
    {
        setpgid(0, 0);

#ifdef __linux__
        // If true_goal dies without cleaning up (kill -9, crash), the camera
        // program is stopped too instead of keeping the webcam busy.
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            _exit(1);
#else
        (void)parent;
#endif

        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);

        // Keep the server socket and GPIO lines out of the camera program.
        for (int fd = 3; fd < maxFd; ++fd)
            close(fd);

        if (chdir(config.dir.c_str()) == 0)
            execl("/bin/sh", "sh", "-c", shell.c_str(), (char*)nullptr);

        _exit(127);
    }

    close(fds[1]);

    if (pid < 0)
    {
        close(fds[0]);
        return -1;
    }

    setpgid(pid, pid);
    out = fds[0];
    return pid;
}

static bool waitChild(pid_t pid, int& status, int ms)
{
    for (int i = 0; i < ms / 50; ++i)
    {
        if (waitpid(pid, &status, WNOHANG) == pid)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// SIGINT lets goal_cam.py release the camera and finish its files; SIGTERM
// and then SIGKILL follow if it does not exit. Returns the wait status.
static int stopChild(pid_t pid)
{
    int status = 0;

    if (waitpid(pid, &status, WNOHANG) == pid)
        return status;

    kill(-pid, SIGINT);
    if (waitChild(pid, status, 3000))
        return status;

    kill(-pid, SIGTERM);
    if (waitChild(pid, status, 1000))
        return status;

    std::cerr << "goal_cam.py did not stop, killing it\n";
    kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
    return status;
}

static std::string describe(int status)
{
    if (WIFEXITED(status))
        return "exit code " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        return "signal " + std::to_string(WTERMSIG(status));
    return "unknown status";
}

static void run()
{
    bool warned = false;
    int failures = 0;

    while (running)
    {
        if (triggerPortInUse())
        {
            if (!warned)
                std::cerr << "UDP port " << config.triggerPort << " is in use: goal_cam.py "
                          << "seems to be running already. Stop it so true_goal can start it.\n";
            warned = true;

            if (callbacks.status)
                callbacks.status("offline");
        }
        else
        {
            warned = false;

            if (callbacks.status)
                callbacks.status("starting");

            int out = -1;
            pid_t pid = spawn(out);

            if (pid < 0)
            {
                std::cerr << "Cannot start goal_cam.py: " << std::strerror(errno) << "\n";
                ++failures;
            }
            else
            {
                std::string buffer;
                long long started = nowMs();
                bool announced = false;

                while (running)
                {
                    checkClip();

                    if (!announced && nowMs() - started > 3000)
                    {
                        announced = true;
                        if (callbacks.status)
                            callbacks.status("yolo");
                    }

                    pollfd p = {out, POLLIN, 0};
                    if (poll(&p, 1, 200) <= 0)
                        continue;

                    char data[1024];
                    ssize_t n = read(out, data, sizeof(data));
                    if (n <= 0)
                        break;

                    buffer.append(data, n);

                    size_t end;
                    while ((end = buffer.find('\n')) != std::string::npos)
                    {
                        std::string line = buffer.substr(0, end);
                        buffer.erase(0, end + 1);
                        std::cout << "[goal_cam] " << line << "\n";
                        parseLine(line);
                    }
                }

                close(out);
                int status = stopChild(pid);
                long long ranMs = nowMs() - started;

                // A program that keeps failing at startup is retried less often.
                failures = ranMs < 30000 ? failures + 1 : 0;

                if (running)
                {
                    std::cerr << "goal_cam.py stopped after " << ranMs / 1000 << " s ("
                              << describe(status) << ")\n";

                    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
                        std::cerr << "Could not run \"" << config.command << "\" in " << config.dir
                                  << ". Check --goalcam-dir and --goalcam-cmd.\n";
                }
            }

            if (callbacks.status)
                callbacks.status("offline");
        }

        int waitS = failures == 0 ? 5 : std::min(60, 5 << std::min(failures - 1, 4));

        if (failures > 0 && running)
            std::cerr << "Restarting goal_cam.py in " << waitS << " s\n";

        for (int i = 0; i < waitS * 10 && running; ++i)
        {
            checkClip();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void start(const Config& c, const Callbacks& cb)
{
    if (running.exchange(true))
        return;

    config = c;
    callbacks = cb;
    worker = std::thread(run);
}

void stop()
{
    if (!running.exchange(false))
        return;

    worker.join();
}

}
