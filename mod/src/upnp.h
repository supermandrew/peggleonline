#pragma once
// Opens the hosting port on the home router with UPnP (Internet Gateway Device), so nobody has to log into
// their router. Runs on its own thread; the UI polls GetResult().
#include <cstdint>
#include <string>

namespace upnp {

struct Result {
    bool done = false;
    bool mapped = false;
    std::string external_ip;  // router's public address, if it reported one
    std::string local_ip;     // this computer's address on the home network
    std::string error;
};

void StartMapping(uint16_t port);
Result GetResult();
void RemoveMapping();  // undo our mapping (blocking, a second or two at most)

}  // namespace upnp
