#pragma once
// Level packs going from one player to the other. A level needs its layout, its background and the pack's shared
// pictures and settings, so a level the joiner lacks comes over as its whole pack: one .pak file for the game's
// levelpacks folder. It's only ever sent to a player who said yes, and never during a duel.
#include <cstdint>
#include <string>

namespace packs {

constexpr uint32_t kMaxSize = 512u << 20;

struct Info {
    std::string file;  // its name in the levelpacks folder
    uint32_t size = 0;
};

bool FindWithLevel(const std::string& level_id, Info& out);  // the pack on this computer that holds that level
bool Acceptable(const Info& offered, std::string& why);      // could this come in? (`why`: if not, for the log)
std::string SizeText(uint32_t bytes);                        // "24.6 MB"

// Sending runs on a thread of its own, as kPackData messages and a closing kPackEnd.
void StartSending(const Info& pack);
void StopSending();     // stops it and doesn't wait: follow with WaitForSender, once the connection can't hold it up
void WaitForSender();
bool Sending();
uint32_t BytesSent();
void SetThrottle(int milliseconds);  // testing: a pause after each piece
void SetCorrupt(bool on);            // testing: what goes out isn't what was read

// Receiving goes into a temporary file the game can't see until all of it has come and been checked.
bool StartReceiving(const Info& pack);
bool Receiving();
bool ReceivePiece(const uint8_t* message, size_t size);  // a kPackData message
bool FinishReceiving(uint32_t crc);                      // the kPackEnd: puts the pack in place
void StopReceiving();                                    // leaves nothing behind
uint32_t BytesReceived();

}  // namespace packs
