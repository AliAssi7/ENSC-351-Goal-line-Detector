#include "camera.h"
#include "detector.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace camera {

static std::atomic<bool> running{false};
static std::thread worker;

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Source {
public:
    virtual ~Source() = default;
    virtual bool open(const Config& config) = 0;
    virtual bool read(Frame& frame) = 0;
    virtual const char* kind() const = 0;

    std::string error;
};

// Synthetic pitch with a vertical goal line at the detector's default
// position and an orange ball repeating four scripted shots:
// goal, ball stopping on the line, save just short of the line, diagonal goal.
class SimSource : public Source {
public:
    bool open(const Config& config) override
    {
        width = config.width;
        height = config.height;
        frameMs = 1000 / std::max(1, config.fps);
        startMs = nextMs = nowMs();
        return true;
    }

    bool read(Frame& frame) override
    {
        long long wait = nextMs - nowMs();
        if (wait > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));

        draw(frame, (nextMs - startMs) / 1000.0);
        nextMs += frameMs;
        return true;
    }

    const char* kind() const override { return "sim"; }

private:
    int width = 0, height = 0, frameMs = 66;
    long long startMs = 0, nextMs = 0;
    uint32_t seed = 12345;

    void draw(Frame& frame, double t)
    {
        frame.width = width;
        frame.height = height;
        frame.rgb.resize(width * height * 3);

        const int lineX = detector::defaultLine(width, height).x1;
        const double r = height / 14.0;
        const double startX = width * 0.12;

        double cycle = std::fmod(t, 16.0);
        int shot = int(cycle / 4);
        double p = std::fmod(cycle, 4.0);

        double targetX[4] = {lineX + r + width * 0.12, double(lineX),
                             lineX - r - 6, lineX + r + width * 0.15};
        double y0 = shot == 3 ? height * 0.3 : height * 0.5;
        double y1 = shot == 3 ? height * 0.65 : height * 0.5;
        bool scored = shot == 0 || shot == 3;

        double k;
        if (p < 1.5)
            k = p / 1.5;
        else if (p < 2.5 || scored)
            k = 1;
        else
            k = std::max(0.0, 1 - (p - 2.5));

        bool visible = p < 3.5;
        double bx = startX + (targetX[shot] - startX) * k;
        double by = y0 + (y1 - y0) * k;

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                uint8_t* px = &frame.rgb[(y * width + x) * 3];
                int c[3];

                if (visible && (x - bx) * (x - bx) + (y - by) * (y - by) <= r * r)
                    c[0] = 235, c[1] = 110, c[2] = 25;
                else if (x >= lineX - 3 && x < lineX)
                    c[0] = c[1] = c[2] = 235;
                else if (x >= lineX)
                    c[0] = 30, c[1] = 100, c[2] = 40;
                else
                    c[0] = 40, c[1] = 130, c[2] = 50;

                seed = seed * 1103515245 + 12345;
                int noise = int((seed >> 16) % 17) - 8;

                for (int i = 0; i < 3; ++i)
                    px[i] = uint8_t(std::min(255, std::max(0, c[i] + noise)));
            }
        }
    }
};

// Decodes a video file with the ffmpeg command-line tool, looping forever.
class FileSource : public Source {
public:
    ~FileSource() override
    {
        if (pipe)
            pclose(pipe);
    }

    bool open(const Config& config) override
    {
        if (config.videoFile.find('\'') != std::string::npos)
        {
            error = "Video path must not contain quotes";
            return false;
        }

        if (!std::ifstream(config.videoFile))
        {
            error = "Cannot open " + config.videoFile;
            return false;
        }

        width = config.width;
        height = config.height;

        // Fatal-only logging: closing the pipe on shutdown is not an error.
        std::string command =
            "ffmpeg -loglevel fatal -re -stream_loop -1 -i '" + config.videoFile +
            "' -vf scale=" + std::to_string(width) + ":" + std::to_string(height) +
            " -r " + std::to_string(config.fps) + " -f rawvideo -pix_fmt rgb24 -";

        pipe = popen(command.c_str(), "r");
        if (!pipe)
            error = "Cannot run ffmpeg";

        return pipe != nullptr;
    }

    bool read(Frame& frame) override
    {
        frame.width = width;
        frame.height = height;
        frame.rgb.resize(width * height * 3);

        return fread(frame.rgb.data(), 1, frame.rgb.size(), pipe) == frame.rgb.size();
    }

    const char* kind() const override { return "file"; }

private:
    FILE* pipe = nullptr;
    int width = 0, height = 0;
};

#ifdef __linux__

// Any V4L2 capture device that offers YUYV or UYVY (all UVC webcams do).
class V4l2Source : public Source {
public:
    ~V4l2Source() override
    {
        if (fd < 0)
            return;

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);

        for (Buffer& b : buffers)
            munmap(b.start, b.length);

        ::close(fd);
    }

    bool open(const Config& config) override
    {
        fd = ::open(config.device.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            return fail(config.device + ": " + std::strerror(errno));

        v4l2_capability cap{};
        if (xioctl(VIDIOC_QUERYCAP, &cap) < 0)
            return fail(config.device + " is not a V4L2 device");

        unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                   : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING))
            return fail(config.device + " cannot stream video capture");

        v4l2_format fmt{};
        for (unsigned format : {V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_UYVY})
        {
            fmt = {};
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            fmt.fmt.pix.width = config.width;
            fmt.fmt.pix.height = config.height;
            fmt.fmt.pix.pixelformat = format;
            fmt.fmt.pix.field = V4L2_FIELD_NONE;

            if (xioctl(VIDIOC_S_FMT, &fmt) == 0 && fmt.fmt.pix.pixelformat == format)
                break;
        }

        unsigned format = fmt.fmt.pix.pixelformat;
        if (format != V4L2_PIX_FMT_YUYV && format != V4L2_PIX_FMT_UYVY)
            return fail(config.device + " does not offer YUYV or UYVY");

        uyvy = format == V4L2_PIX_FMT_UYVY;
        width = fmt.fmt.pix.width;
        height = fmt.fmt.pix.height;
        stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : width * 2;

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = config.fps;
        xioctl(VIDIOC_S_PARM, &parm);

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (xioctl(VIDIOC_REQBUFS, &req) < 0 || req.count < 2)
            return fail("Cannot allocate camera buffers");

        for (unsigned i = 0; i < req.count; ++i)
        {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (xioctl(VIDIOC_QUERYBUF, &buf) < 0)
                return fail("Cannot query camera buffer");

            void* start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, buf.m.offset);
            if (start == MAP_FAILED)
                return fail("Cannot map camera buffer");

            buffers.push_back({start, buf.length});

            if (xioctl(VIDIOC_QBUF, &buf) < 0)
                return fail("Cannot queue camera buffer");
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(VIDIOC_STREAMON, &type) < 0)
            return fail("Cannot start camera stream");

        std::cout << "Camera " << config.device << ": " << width << "x" << height
                  << (uyvy ? " UYVY" : " YUYV") << "\n";
        return true;
    }

    bool read(Frame& frame) override
    {
        // Up to 2 s without a frame counts as a lost camera.
        for (int tries = 0; tries < 4 && running; ++tries)
        {
            pollfd p = {fd, POLLIN, 0};
            int n = poll(&p, 1, 500);

            if (n < 0 && errno == EINTR)
                continue;

            if (n < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
                return false;

            if (n == 0)
                continue;

            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            if (xioctl(VIDIOC_DQBUF, &buf) < 0)
            {
                if (errno == EAGAIN)
                    continue;
                return false;
            }

            convert(static_cast<const uint8_t*>(buffers[buf.index].start), frame);
            return xioctl(VIDIOC_QBUF, &buf) == 0;
        }

        return false;
    }

    const char* kind() const override { return "live"; }

private:
    struct Buffer {
        void* start;
        size_t length;
    };

    int fd = -1;
    int width = 0, height = 0, stride = 0;
    bool uyvy = false;
    std::vector<Buffer> buffers;

    bool fail(const std::string& message)
    {
        error = message;
        return false;
    }

    int xioctl(unsigned long request, void* arg)
    {
        int rc;
        do
            rc = ioctl(fd, request, arg);
        while (rc < 0 && errno == EINTR);
        return rc;
    }

    static uint8_t clamp(int v)
    {
        return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
    }

    void convert(const uint8_t* src, Frame& frame)
    {
        frame.width = width;
        frame.height = height;
        frame.rgb.resize(width * height * 3);
        uint8_t* out = frame.rgb.data();

        const int yi = uyvy ? 1 : 0, ui = uyvy ? 0 : 1, vi = uyvy ? 2 : 3;

        for (int y = 0; y < height; ++y)
        {
            const uint8_t* row = src + y * stride;

            for (int x = 0; x < width; x += 2, row += 4)
            {
                int d = row[ui] - 128;
                int e = row[vi] - 128;

                for (int k = 0; k < 2; ++k)
                {
                    int c = 298 * (row[yi + 2 * k] - 16);
                    *out++ = clamp((c + 409 * e + 128) >> 8);
                    *out++ = clamp((c - 100 * d - 208 * e + 128) >> 8);
                    *out++ = clamp((c + 516 * d + 128) >> 8);
                }
            }
        }
    }
};

#endif

static std::unique_ptr<Source> makeSource(const Config& config)
{
    if (config.simulate)
        return std::make_unique<SimSource>();

    if (!config.videoFile.empty())
        return std::make_unique<FileSource>();

#ifdef __linux__
    return std::make_unique<V4l2Source>();
#else
    return nullptr;
#endif
}

static void pause(int ms)
{
    for (int i = 0; i < ms / 100 && running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

static void run(Config config,
                std::function<void(std::shared_ptr<Frame>)> onFrame,
                std::function<void(const std::string&)> onStatus)
{
    std::string lastError;

    while (running)
    {
        std::unique_ptr<Source> source = makeSource(config);

        if (!source || !source->open(config))
        {
            std::string error = source ? source->error : "No camera support on this platform";
            if (error != lastError)
                std::cerr << "Camera unavailable: " << error << "\n";

            lastError = error;
            source.reset();
            onStatus("offline");
            pause(2000);
            continue;
        }

        lastError.clear();
        onStatus(source->kind());

        while (running)
        {
            auto frame = std::make_shared<Frame>();

            if (!source->read(*frame))
                break;

            frame->timeMs = nowMs();
            onFrame(frame);
        }

        source.reset();
        onStatus("offline");

        if (running)
        {
            std::cerr << "Camera stopped, reopening\n";
            pause(1000);
        }
    }
}

void start(const Config& config,
           std::function<void(std::shared_ptr<Frame>)> onFrame,
           std::function<void(const std::string&)> onStatus)
{
    if (running.exchange(true))
        return;

    worker = std::thread(run, config, onFrame, onStatus);
}

void stop()
{
    if (!running.exchange(false))
        return;

    worker.join();
}

}
