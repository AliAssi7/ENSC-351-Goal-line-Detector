#ifndef NETWORK_H
#define NETWORK_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace network {

// Connects (and reconnects) to the laptop server in the background.
// Callbacks run on the network thread.
void start(const std::string& host, int port,
           std::function<void(const std::string&)> onLine,
           std::function<void()> onConnect);

// Both drop the message if the server is not connected.
void send(const std::string& line);

// Sends header + "\n" + data. Each slot only keeps the newest frame, so a
// slow link drops old frames instead of queueing them.
void sendFrame(int slot, const std::string& header,
               std::shared_ptr<const std::vector<uint8_t>> data);

bool connected();

// This board's IP address on the current server connection ("" if none).
std::string localAddress();
void stop();

}

#endif
