#pragma once
// One TCP connection between two copies of the game. The host listens, the other side connects.
// A worker thread does the blocking socket work; the game thread polls Receive() once per frame.
#include <cstdint>
#include <string>
#include <vector>

namespace net {

constexpr uint16_t kDefaultPort = 28008;

enum class State { Idle, Listening, Connecting, Connected, Closed };

struct Message {
    uint8_t type = 0;
    std::vector<uint8_t> data;
};

void StartHost(uint16_t port);
void StartJoin(const std::string& host, uint16_t port);
void Shutdown();  // close everything; safe to call in any state

State GetState();
std::string GetError();    // why the connection is Closed
std::string PeerAddress();
uint32_t MillisSinceReceive();

bool Send(uint8_t type, const void* data = nullptr, size_t len = 0);
bool Receive(Message& out);  // non-blocking

// Parse "host", "host:port" or "[v6]:port"; returns false if empty.
bool ParseAddress(const std::string& text, std::string& host, uint16_t& port);

}  // namespace net
