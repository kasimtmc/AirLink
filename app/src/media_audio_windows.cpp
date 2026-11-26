#include "platform.hpp"

#include <windows.h>
#include <mmsystem.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace airlink::platform {
namespace {

struct Packet {
    WAVEHDR header{};
    std::vector<uint8_t> bytes;
};

struct Player {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<std::vector<uint8_t>> pending;
    std::jthread worker;
};

Player& player() { static Player p; return p; }

void run(std::stop_token stop) {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 96000;
    HWAVEOUT device = nullptr;
    if (waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
    auto& p = player();
    std::deque<std::unique_ptr<Packet>> playing;
    while (!stop.stop_requested()) {
        for (auto it = playing.begin(); it != playing.end();) {
            if ((*it)->header.dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(device, &(*it)->header, sizeof(WAVEHDR));
                it = playing.erase(it);
            } else ++it;
        }
        std::vector<uint8_t> bytes;
        {
            std::unique_lock lock(p.mutex);
            p.wake.wait_for(lock, stop, std::chrono::milliseconds(20), [&] { return !p.pending.empty(); });
            if (stop.stop_requested()) break;
            if (p.pending.empty()) continue;
            bytes = std::move(p.pending.front());
            p.pending.pop_front();
        }
        auto packet = std::make_unique<Packet>();
        packet->bytes = std::move(bytes);
        packet->header.lpData = reinterpret_cast<LPSTR>(packet->bytes.data());
        packet->header.dwBufferLength = static_cast<DWORD>(packet->bytes.size());
        if (waveOutPrepareHeader(device, &packet->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) continue;
        if (waveOutWrite(device, &packet->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(device, &packet->header, sizeof(WAVEHDR));
            continue;
        }
        playing.push_back(std::move(packet));
    }
    waveOutReset(device);
    for (auto& packet : playing) waveOutUnprepareHeader(device, &packet->header, sizeof(WAVEHDR));
    waveOutClose(device);
}

}  // namespace

bool canPlayMediaAudio() { return waveOutGetNumDevs() > 0; }

void playMediaAudio(const std::vector<uint8_t>& pcm) {
    if (pcm.empty() || pcm.size() > 19200 || pcm.size() % 2) return;
    auto& p = player();
    {
        std::lock_guard lock(p.mutex);
        if (p.pending.size() == 3) p.pending.pop_front();
        p.pending.push_back(pcm);
    }
    if (!p.worker.joinable()) p.worker = std::jthread([](std::stop_token stop) { run(stop); });
    p.wake.notify_one();
}

void stopMediaAudio() {
    auto& p = player();
    if (p.worker.joinable()) {
        p.worker.request_stop();
        p.wake.notify_all();
        p.worker.join();
    }
    std::lock_guard lock(p.mutex);
    p.pending.clear();
}

}  // namespace airlink::platform
