#include "network.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace network {

using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

static const int kSlots = 2;
static const long long kTimeoutMs = 6000;   // The server sends PING every 2 s
static const size_t kMaxQueuedText = 256 * 1024;

struct Message {
    std::string head;
    Bytes data;
    size_t offset = 0;

    size_t size() const { return head.size() + (data ? data->size() : 0); }
};

static std::thread worker;
static std::atomic<bool> running{false};
static std::mutex mutex;
static int sock = -1;
static int wakePipe[2] = {-1, -1};
static std::string localIp;
static std::deque<std::string> textQueue;
static size_t queuedText = 0;
static Message slots[kSlots];
static bool slotFull[kSlots] = {};

static long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void wake()
{
    char c = 1;
    if (write(wakePipe[1], &c, 1) < 0) {}
}

static int connectTo(const std::string& host, int port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;

    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                    &hints, &result) != 0)
        return -1;

    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);

    if (fd >= 0)
    {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);

#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

        // Non-blocking connect so shutdown is never stuck behind a TCP timeout.
        bool connected = connect(fd, result->ai_addr, result->ai_addrlen) == 0;

        if (!connected && errno == EINPROGRESS)
        {
            pollfd p = {fd, POLLOUT, 0};
            int error = 0;
            socklen_t len = sizeof(error);

            connected = poll(&p, 1, 2000) == 1 &&
                        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 &&
                        error == 0;
        }

        if (!connected)
        {
            close(fd);
            fd = -1;
        }
    }

    freeaddrinfo(result);
    return fd;
}

// The board's own address on the link to the server, which is the one the
// dashboard can reach (the server may see a NAT address instead).
static std::string socketAddress(int fd)
{
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    char host[NI_MAXHOST] = "";

    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof(host),
                    nullptr, 0, NI_NUMERICHOST) != 0)
        return "";

    std::string ip = host;
    return ip.rfind("::ffff:", 0) == 0 ? ip.substr(7) : ip;
}

static void resetQueues()
{
    textQueue.clear();
    queuedText = 0;

    for (int i = 0; i < kSlots; ++i)
    {
        slots[i] = Message();
        slotFull[i] = false;
    }
}

// Text keeps its order and goes first; replay frames before live frames.
static bool nextMessage(Message& out)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (!textQueue.empty())
    {
        out = Message();
        out.head = std::move(textQueue.front());
        queuedText -= out.head.size();
        textQueue.pop_front();
        return true;
    }

    for (int i = kSlots - 1; i >= 0; --i)
    {
        if (slotFull[i])
        {
            out = std::move(slots[i]);
            slotFull[i] = false;
            return true;
        }
    }

    return false;
}

// Returns false if the connection failed.
static bool writeSome(int fd, Message& out, bool& busy)
{
    while (true)
    {
        if (!busy)
        {
            if (!nextMessage(out))
                return true;
            busy = true;
        }

        const char* data;
        size_t length;

        if (out.offset < out.head.size())
        {
            data = out.head.data() + out.offset;
            length = out.head.size() - out.offset;
        }
        else
        {
            size_t at = out.offset - out.head.size();
            data = reinterpret_cast<const char*>(out.data->data()) + at;
            length = out.data->size() - at;
        }

        ssize_t sent = ::send(fd, data, length, MSG_NOSIGNAL);

        if (sent < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;

        out.offset += sent;

        if (out.offset == out.size())
            busy = false;
    }
}

static void session(int fd, const std::function<void(const std::string&)>& onLine)
{
    std::string buffer;
    Message out;
    bool busy = false;
    long long lastReceived = nowMs();

    while (running)
    {
        if (!writeSome(fd, out, busy))
            return;

        bool pendingOutput = busy;
        if (!pendingOutput)
        {
            std::lock_guard<std::mutex> lock(mutex);
            pendingOutput = !textQueue.empty() || slotFull[0] || slotFull[1];
        }

        pollfd p[2] = {{fd, short(POLLIN | (pendingOutput ? POLLOUT : 0)), 0},
                       {wakePipe[0], POLLIN, 0}};

        int n = poll(p, 2, 200);

        if (n < 0 && errno != EINTR)
            return;

        if (nowMs() - lastReceived > kTimeoutMs)
        {
            std::cerr << "Dashboard server not responding\n";
            return;
        }

        if (n <= 0)
            continue;

        if (p[1].revents & POLLIN)
        {
            char drain[64];
            while (read(wakePipe[0], drain, sizeof(drain)) > 0) {}
        }

        if (p[0].revents & (POLLERR | POLLHUP))
            return;

        if (!(p[0].revents & POLLIN))
            continue;

        char data[512];
        ssize_t received = recv(fd, data, sizeof(data), 0);

        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;

        if (received <= 0)
            return;

        lastReceived = nowMs();
        buffer.append(data, received);

        size_t end;
        while ((end = buffer.find('\n')) != std::string::npos)
        {
            std::string line = buffer.substr(0, end);
            buffer.erase(0, end + 1);

            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            onLine(line);
        }

        if (buffer.size() > 4096)
            return;
    }
}

static void run(std::string host, int port,
                std::function<void(const std::string&)> onLine,
                std::function<void()> onConnect)
{
    while (running)
    {
        int fd = connectTo(host, port);

        if (fd < 0)
        {
            for (int i = 0; i < 20 && running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            resetQueues();
            sock = fd;
            localIp = socketAddress(fd);
        }

        std::cout << "Connected to dashboard server\n";

        if (onConnect)
            onConnect();

        session(fd, onLine);

        {
            std::lock_guard<std::mutex> lock(mutex);
            sock = -1;
            resetQueues();
        }

        close(fd);

        if (running)
            std::cout << "Dashboard server disconnected, reconnecting\n";
    }
}

void start(const std::string& host, int port,
           std::function<void(const std::string&)> onLine,
           std::function<void()> onConnect)
{
    if (running.exchange(true))
        return;

    if (pipe(wakePipe) == 0)
    {
        for (int fd : wakePipe)
        {
            fcntl(fd, F_SETFL, O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }

    worker = std::thread(run, host, port, onLine, onConnect);
}

void send(const std::string& line)
{
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (sock < 0 || queuedText > kMaxQueuedText)
            return;

        textQueue.push_back(line + "\n");
        queuedText += line.size() + 1;
    }

    wake();
}

void sendFrame(int slot, const std::string& header, Bytes data)
{
    if (slot < 0 || slot >= kSlots || !data)
        return;

    {
        std::lock_guard<std::mutex> lock(mutex);

        if (sock < 0)
            return;

        slots[slot] = Message();
        slots[slot].head = header + "\n";
        slots[slot].data = data;
        slotFull[slot] = true;
    }

    wake();
}

std::string localAddress()
{
    std::lock_guard<std::mutex> lock(mutex);
    return localIp;
}

bool connected()
{
    std::lock_guard<std::mutex> lock(mutex);
    return sock >= 0;
}

void stop()
{
    if (!running.exchange(false))
        return;

    wake();
    worker.join();

    close(wakePipe[0]);
    close(wakePipe[1]);
    wakePipe[0] = wakePipe[1] = -1;
}

}
