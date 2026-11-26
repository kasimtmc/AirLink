// Windows: the paired phone's Bluetooth audio, implemented in the Rust core
// (airlink-desktop's bt_audio over WinRT): the call as the phone's HFP hands-free unit, and the
// player's media as an A2DP sink.
#include "platform.hpp"

#include "airlink.h"

namespace airlink::platform {
namespace {

std::string take(char* s) {
    std::string out = s ? s : "";
    airlink_free_string(s);
    return out;
}

}  // namespace

bool canBridgeCallAudio() { return airlink_win_call_audio_supported(); }
void startCallAudio(const std::string& phoneName) { airlink_win_call_audio_start(phoneName.c_str()); }
void stopCallAudio() { airlink_win_call_audio_stop(); }

CallAudioPhase callAudioPhase() {
    switch (airlink_win_call_audio_phase(nullptr)) {
        case 1: return CallAudioPhase::Connecting;
        case 2: return CallAudioPhase::WaitingForRoute;
        case 3: return CallAudioPhase::Active;
        case 4: return CallAudioPhase::Error;
        default: return CallAudioPhase::Off;
    }
}

std::string callAudioStatus() {
    char* status = nullptr;
    airlink_win_call_audio_phase(&status);
    return take(status);
}

void setPlayerBluetoothPhone(const std::string& phoneName) { airlink_win_player_bluetooth(phoneName.c_str()); }
std::string playerBluetoothStatus() { return take(airlink_win_player_bluetooth_status()); }

}  // namespace airlink::platform
