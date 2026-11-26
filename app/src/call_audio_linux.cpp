// A Linux computer is the HFP Hands-Free unit; Android remains the Audio Gateway.
// PipeWire/WirePlumber owns the Bluetooth SCO transport. AirLink only links its
// call source/sink to the current desktop microphone/speakers while requested.

#include "platform.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <pulse/simple.h>
#include <pulse/error.h>

extern char** environ;

namespace airlink::platform {
namespace {

struct CommandResult { int status = -1; std::string output; };

// Arguments are passed directly, never through a shell: device names are untrusted.
CommandResult command(std::initializer_list<std::string> args) {
    if (args.size() == 0) return {};
    int pipefd[2];
    if (pipe(pipefd) != 0) return {};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (spawned != 0) { close(pipefd[0]); return {}; }
    CommandResult result;
    std::array<char, 2048> buffer{};
    for (;;) {
        ssize_t size = read(pipefd[0], buffer.data(), buffer.size());
        if (size > 0) {
            if (result.output.size() < 65536) result.output.append(buffer.data(), std::min<size_t>(size, 65536 - result.output.size()));
        } else if (size == 0) break;
        else if (errno != EINTR) break;
    }
    close(pipefd[0]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited == pid && WIFEXITED(status)) result.status = WEXITSTATUS(status);
    return result;
}

std::string trim(std::string text) {
    const auto end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return {};
    const auto begin = text.find_first_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool validAddress(std::string_view address) {
    if (address.size() != 17) return false;
    for (size_t i = 0; i < address.size(); ++i)
        if (i % 3 == 2 ? address[i] != ':' : !std::isxdigit(static_cast<unsigned char>(address[i]))) return false;
    return true;
}

constexpr const char* hfpGateway = "0000111f-0000-1000-8000-00805f9b34fb";
constexpr const char* a2dpSource = "0000110a-0000-1000-8000-00805f9b34fb";

std::optional<std::string> phoneAddress(const std::string& name, const char* profile = hfpGateway,
                                        bool allowSingleFallback = true) {
    auto devices = command({"bluetoothctl", "devices", "Paired"});
    if (devices.status != 0) return std::nullopt;
    std::optional<std::string> phone, matching;
    size_t phoneCount = 0, matchCount = 0;
    size_t begin = 0;
    while (begin < devices.output.size()) {
        size_t end = devices.output.find('\n', begin);
        if (end == std::string::npos) end = devices.output.size();
        std::string_view line(devices.output.data() + begin, end - begin);
        if (line.starts_with("Device ") && line.size() > 25) {
            std::string address(line.substr(7, 17));
            if (validAddress(address)) {
                auto info = command({"bluetoothctl", "info", address});
                if (info.status == 0 && info.output.find(profile) != std::string::npos) {
                    phone = address;
                    ++phoneCount;
                    if (trim(std::string(line.substr(25))) == name) {
                        matching = address;
                        ++matchCount;
                    }
                }
            }
        }
        begin = end + 1;
    }
    if (matchCount == 1) return matching;
    if (allowSingleFallback && matchCount == 0 && phoneCount == 1) return phone;
    return std::nullopt;
}

std::string bluezNode(const char* kind, const std::string& address) {
    auto list = command({"pactl", "list", kind, "short"});
    if (list.status != 0) return {};
    std::string suffix = address;
    std::replace(suffix.begin(), suffix.end(), ':', '_');
    const std::string prefix = std::string(kind == std::string("sources") ? "bluez_input." : "bluez_output.") + suffix;
    size_t begin = 0;
    while (begin < list.output.size()) {
        size_t end = list.output.find('\n', begin);
        if (end == std::string::npos) end = list.output.size();
        std::string_view line(list.output.data() + begin, end - begin);
        size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            size_t next = line.find('\t', tab + 1);
            std::string_view node = line.substr(tab + 1, next - tab - 1);
            if (node.starts_with(prefix)) return std::string(node);
        }
        begin = end + 1;
    }
    return {};
}

int loadLoop(const std::string& source, const std::string& sink) {
    auto result = command({"pactl", "load-module", "module-loopback", "source=" + source,
                           "sink=" + sink, "latency_msec=40"});
    if (result.status != 0) return -1;
    try { return std::stoi(trim(result.output)); } catch (...) { return -1; }
}

void unload(int id) {
    if (id >= 0) command({"pactl", "unload-module", std::to_string(id)});
}

struct State {
    std::mutex mutex;
    std::jthread worker;
    CallAudioPhase phase = CallAudioPhase::Off;
    std::string status;
};
State& state() { static State s; return s; }

void set(CallAudioPhase phase, std::string status) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.phase = phase;
    s.status = std::move(status);
}

void bridge(std::stop_token stop, std::string phoneName) {
    auto address = phoneAddress(phoneName);
    if (!address) return set(CallAudioPhase::Error, "Eşleşmiş Bluetooth telefon bulunamadı");
    if (bluezNode("sources", *address).empty() || bluezNode("sinks", *address).empty()) {
        // The remote phone is the HFP Audio Gateway. Connecting every profile
        // can also route its music to the PC through A2DP.
        command({"bluetoothctl", "connect", *address, hfpGateway});
    }
    if (stop.stop_requested()) return set(CallAudioPhase::Off, {});
    set(CallAudioPhase::WaitingForRoute, "Telefonun Bluetooth görüşme ses yolu bekleniyor");
    std::string incoming, outgoing;
    for (int tries = 0; tries < 80 && !stop.stop_requested(); ++tries) {
        incoming = bluezNode("sources", *address);
        outgoing = bluezNode("sinks", *address);
        if (!incoming.empty() && !outgoing.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (stop.stop_requested()) return set(CallAudioPhase::Off, {});
    if (incoming.empty() || outgoing.empty())
        return set(CallAudioPhase::Error, "Telefonun HFP ses uçları açılmadı; görüşmede Bluetooth'u seçin");
    auto speaker = command({"pactl", "get-default-sink"});
    auto microphone = command({"pactl", "get-default-source"});
    if (speaker.status != 0 || microphone.status != 0)
        return set(CallAudioPhase::Error, "Bilgisayarın mikrofonu veya hoparlörü bulunamadı");
    int hear = loadLoop(incoming, trim(speaker.output));
    if (hear < 0) return set(CallAudioPhase::Error, "Görüşme sesi hoparlöre bağlanamadı");
    int speak = loadLoop(trim(microphone.output), outgoing);
    if (speak < 0) {
        unload(hear);
        return set(CallAudioPhase::Error, "Mikrofon telefona bağlanamadı");
    }
    set(CallAudioPhase::Active, "Görüşme sesi bilgisayarda");
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (bluezNode("sources", *address).empty() || bluezNode("sinks", *address).empty()) break;
    }
    unload(speak);
    unload(hear);
    set(CallAudioPhase::Off, {});
}

}  // namespace

bool canBridgeCallAudio() { return command({"pactl", "info"}).status == 0; }

void startCallAudio(const std::string& phoneName) {
    auto& s = state();
    if (s.worker.joinable()) { s.worker.request_stop(); s.worker.join(); }
    set(CallAudioPhase::Connecting, "Bluetooth telefona bağlanıyor");
    s.worker = std::jthread([phoneName](std::stop_token stop) { bridge(stop, phoneName); });
}

void stopCallAudio() {
    auto& s = state();
    if (s.worker.joinable()) s.worker.request_stop();
}

CallAudioPhase callAudioPhase() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.phase;
}

std::string callAudioStatus() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.status;
}

namespace {
struct PlayerBluetooth {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::string desiredPhone;
    std::string status;
    std::jthread worker;
};

PlayerBluetooth& playerBluetooth() { static PlayerBluetooth state; return state; }

void runPlayerBluetooth(std::stop_token stop) {
    auto& state = playerBluetooth();
    std::string connected;
    while (!stop.stop_requested()) {
        std::string phone;
        {
            std::unique_lock lock(state.mutex);
            state.wake.wait(lock, stop, [&] { return !state.desiredPhone.empty(); });
            if (stop.stop_requested()) break;
            phone = state.desiredPhone;
            state.status = "Bluetooth medya bağlantısı kuruluyor";
        }
        auto address = phoneAddress(phone, a2dpSource, false);
        if (address && !stop.stop_requested()) {
            auto result = command({"bluetoothctl", "--timeout", "6", "connect", *address, a2dpSource});
            if (result.status == 0) connected = *address;
        }
        {
            std::unique_lock lock(state.mutex);
            if (state.desiredPhone == phone) {
                if (connected.empty()) {
                    state.status = address ? "Bluetooth medya bağlantısı kurulamadı"
                                           : "Linux'ta aynı adla eşlenmiş Bluetooth telefon bulunamadı";
                    state.wake.wait_for(lock, stop, std::chrono::seconds(8),
                                        [&] { return state.desiredPhone != phone; });
                } else {
                    state.status = "Bluetooth medya profili bağlandı";
                    state.wake.wait(lock, stop, [&] { return state.desiredPhone != phone; });
                }
            }
        }
        if (!connected.empty()) {
            command({"bluetoothctl", "--timeout", "6", "disconnect", connected, a2dpSource});
            connected.clear();
        }
    }
}

struct MediaPlayer {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<std::vector<uint8_t>> packets;
    std::jthread worker;
};
MediaPlayer& mediaPlayer() { static MediaPlayer player; return player; }

void playPackets(std::stop_token stop) {
    auto& player = mediaPlayer();
    pa_sample_spec spec{PA_SAMPLE_S16LE, 48000, 1};
    int error = 0;
    pa_buffer_attr buffer{};
    buffer.maxlength = uint32_t(-1);
    buffer.tlength = pa_usec_to_bytes(250000, &spec);
    buffer.prebuf = uint32_t(-1);
    buffer.minreq = uint32_t(-1);
    buffer.fragsize = uint32_t(-1);
    pa_simple* stream = pa_simple_new(nullptr, "AirLink", PA_STREAM_PLAYBACK, nullptr,
                                      "Telefon medya sesi", &spec, nullptr, &buffer, &error);
    if (!stream) return;
    while (!stop.stop_requested()) {
        std::vector<uint8_t> packet;
        {
            std::unique_lock lock(player.mutex);
            player.wake.wait(lock, stop, [&] { return !player.packets.empty(); });
            if (stop.stop_requested()) break;
            packet = std::move(player.packets.front());
            player.packets.pop_front();
        }
        if (pa_simple_write(stream, packet.data(), packet.size(), &error) < 0) break;
    }
    pa_simple_free(stream);
}
}  // namespace

void setPlayerBluetoothPhone(const std::string& phoneName) {
    auto& state = playerBluetooth();
    {
        std::lock_guard lock(state.mutex);
        if (state.desiredPhone == phoneName) return;
        state.desiredPhone = phoneName;
        state.status = phoneName.empty() ? "" : "Bluetooth medya bağlantısı kuruluyor";
        if (!phoneName.empty() && !state.worker.joinable())
            state.worker = std::jthread(runPlayerBluetooth);
    }
    state.wake.notify_all();
}

std::string playerBluetoothStatus() {
    auto& state = playerBluetooth();
    std::lock_guard lock(state.mutex);
    return state.status;
}

bool canPlayMediaAudio() { return command({"pactl", "info"}).status == 0; }

void playMediaAudio(const std::vector<uint8_t>& pcm) {
    if (pcm.empty() || pcm.size() > 19200 || pcm.size() % 2) return;
    auto& player = mediaPlayer();
    {
        std::lock_guard lock(player.mutex);
        if (player.packets.size() == 3) player.packets.pop_front();
        player.packets.push_back(pcm);
    }
    if (!player.worker.joinable())
        player.worker = std::jthread([](std::stop_token stop) { playPackets(stop); });
    player.wake.notify_one();
}

void stopMediaAudio() {
    auto& player = mediaPlayer();
    if (player.worker.joinable()) {
        player.worker.request_stop();
        player.wake.notify_all();
        player.worker.join();
    }
    std::lock_guard lock(player.mutex);
    player.packets.clear();
}

}  // namespace airlink::platform
