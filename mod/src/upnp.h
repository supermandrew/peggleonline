#pragma once
// Opens the hosting port on the home router with UPnP (Internet Gateway Device), so nobody has to log into
// their router. Runs on its own thread; the UI polls GetResult().
#include <cstdint>
#include <string>

namespace upnp {

struct Result {
    bool done = false;
    bool mapped = false;
    bool reachable = false;   // not mapped, but connecting to the public address comes back here: forwarded by hand
    std::string external_ip;  // the public address, from the router or else from an address service
    std::string local_ip;     // this computer's address on the home network
    std::string error;
};

void StartMapping(uint16_t port);
Result GetResult();
void RemoveMapping();  // undo our mapping (blocking, a second or two at most)

}  // namespace upnp
