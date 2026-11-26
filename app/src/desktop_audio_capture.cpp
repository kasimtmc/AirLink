#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#else
#include <pulse/pulseaudio.h>
#endif

namespace airlink::platform {
namespace {

using namespace std::chrono_literals;
constexpr size_t packetBytes = 9600; // 100 ms of 48 kHz, mono, signed 16-bit PCM.

struct Capture {
    std::mutex mutex;
    std::vector<uint8_t> latest;
    std::atomic_bool failed{false};
    std::jthread worker;
};
Capture& capture() { static Capture state; return state; }

struct PacketWriter {
    std::vector<uint8_t> partial;

    void append(const uint8_t* data, size_t size) {
        while (size > 0) {
            const size_t count = std::min(size, packetBytes - partial.size());
            if (data) {
                partial.insert(partial.end(), data, data + count);
                data += count;
            } else {
                partial.resize(partial.size() + count, 0);
            }
            size -= count;
            if (partial.size() == packetBytes) {
                auto& state = capture();
                std::lock_guard lock(state.mutex);
                partial.swap(state.latest);
                partial.clear();
            }
        }
    }
};

#if defined(_WIN32)

template<class T> struct Release {
    void operator()(T* value) const { value->Release(); }
};

class EndpointMute {
    std::unique_ptr<IAudioEndpointVolume, Release<IAudioEndpointVolume>> endpoint;
    bool ownsMute = false;

public:
    bool apply(IMMDevice* device) {
        IAudioEndpointVolume* raw = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                                    nullptr, reinterpret_cast<void**>(&raw)))) return false;
        endpoint.reset(raw);
        BOOL muted = FALSE;
        if (FAILED(endpoint->GetMute(&muted))) return false;
        if (muted) return true;
        if (FAILED(endpoint->SetMute(TRUE, nullptr))) return false;
        ownsMute = true;
        return true;
    }

    ~EndpointMute() {
        if (ownsMute) endpoint->SetMute(FALSE, nullptr);
    }
};

bool captureWasapi(std::stop_token stop) {
    IMMDeviceEnumerator* enumeratorRaw = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumeratorRaw)))) return false;
    std::unique_ptr<IMMDeviceEnumerator, Release<IMMDeviceEnumerator>> enumerator(enumeratorRaw);
    IMMDevice* deviceRaw = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &deviceRaw))) return false;
    std::unique_ptr<IMMDevice, Release<IMMDevice>> device(deviceRaw);
    IAudioClient* clientRaw = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                reinterpret_cast<void**>(&clientRaw)))) return false;
    std::unique_ptr<IAudioClient, Release<IAudioClient>> client(clientRaw);
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 96000;
    constexpr DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &format, nullptr)))
        return false;
    IAudioCaptureClient* readerRaw = nullptr;
    if (FAILED(client->GetService(IID_PPV_ARGS(&readerRaw)))) return false;
    std::unique_ptr<IAudioCaptureClient, Release<IAudioCaptureClient>> reader(readerRaw);
    if (FAILED(client->Start())) return false;
    EndpointMute localOutput;
    if (!localOutput.apply(device.get())) {
        client->Stop();
        return false;
    }

    PacketWriter writer;
    bool ok = true;
    while (!stop.stop_requested()) {
        UINT32 pending = 0;
        if (FAILED(reader->GetNextPacketSize(&pending))) { ok = false; break; }
        if (!pending) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD packetFlags = 0;
        if (FAILED(reader->GetBuffer(&data, &frames, &packetFlags, nullptr, nullptr))) {
            ok = false;
            break;
        }
        writer.append((packetFlags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data,
                      size_t(frames) * format.nBlockAlign);
        if (FAILED(reader->ReleaseBuffer(frames))) { ok = false; break; }
    }
    client->Stop();
    return ok;
}

bool runCapture(std::stop_token stop) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
    const bool ok = captureWasapi(stop);
    CoUninitialize();
    return ok;
}

#else

// Pulse monitors can follow sink mute. Move playback to a silent sink and capture
// that sink instead of muting the same sink we are trying to record.
class PulseScreenOutput {
    pa_mainloop* loop;
    pa_context* context;
    uint32_t module = PA_INVALID_INDEX;
    uint32_t originalSink = PA_INVALID_INDEX;
    uint32_t sharedSink = PA_INVALID_INDEX;
    std::string name;

    bool finish(pa_operation* operation) {
        if (!operation) return false;
        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING &&
               std::chrono::steady_clock::now() < deadline) {
            if (pa_mainloop_iterate(loop, 0, nullptr) < 0) break;
            std::this_thread::sleep_for(2ms);
        }
        const bool done = pa_operation_get_state(operation) == PA_OPERATION_DONE;
        if (!done) pa_operation_cancel(operation);
        pa_operation_unref(operation);
        return done;
    }

    uint32_t findSink(const char* target) {
        uint32_t index = PA_INVALID_INDEX;
        finish(pa_context_get_sink_info_by_name(context, target,
            [](pa_context*, const pa_sink_info* info, int end, void* data) {
                if (!end && info) *static_cast<uint32_t*>(data) = info->index;
            }, &index));
        return index;
    }

    bool moveStreams(uint32_t from, uint32_t to, bool excludeAirLink) {
        struct Inputs {
            uint32_t sink;
            bool excludeAirLink;
            bool complete = false;
            std::vector<uint32_t> indexes;
        } inputs{from, excludeAirLink, false, {}};
        if (!finish(pa_context_get_sink_input_info_list(context,
                [](pa_context*, const pa_sink_input_info* info, int end, void* data) {
                    auto& list = *static_cast<Inputs*>(data);
                    if (end) {
                        list.complete = end > 0;
                        return;
                    }
                    if (!info || info->sink != list.sink) return;
                    const char* app = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
                    if (list.excludeAirLink && app && std::string_view(app) == "AirLink") return;
                    list.indexes.push_back(info->index);
                }, &inputs)) || !inputs.complete) return false;
        for (uint32_t index : inputs.indexes)
            finish(pa_context_move_sink_input_by_index(context, index, to, nullptr, nullptr));
        return true;
    }

public:
    PulseScreenOutput(pa_mainloop* eventLoop, pa_context* connection) : loop(eventLoop), context(connection) {}

    bool create() {
        originalSink = findSink("@DEFAULT_SINK@");
        if (originalSink == PA_INVALID_INDEX) return false;
        name = "airlink_share_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string arguments = "sink_name=" + name + " sink_properties=device.description=AirLink";
        if (!finish(pa_context_load_module(context, "module-null-sink", arguments.c_str(),
                [](pa_context*, uint32_t index, void* data) { *static_cast<uint32_t*>(data) = index; },
                &module)) || module == PA_INVALID_INDEX) return false;
        sharedSink = findSink(name.c_str());
        return sharedSink != PA_INVALID_INDEX;
    }

    std::string monitor() const { return name + ".monitor"; }

    bool redirect() { return moveStreams(originalSink, sharedSink, true); }

    ~PulseScreenOutput() {
        if (module == PA_INVALID_INDEX) return;
        if (sharedSink != PA_INVALID_INDEX) moveStreams(sharedSink, originalSink, false);
        finish(pa_context_unload_module(context, module, nullptr, nullptr));
    }
};

bool runCapture(std::stop_token stop) {
    std::unique_ptr<pa_mainloop, decltype(&pa_mainloop_free)> loop(pa_mainloop_new(), pa_mainloop_free);
    if (!loop) return false;
    std::unique_ptr<pa_context, decltype(&pa_context_unref)> context(
        pa_context_new(pa_mainloop_get_api(loop.get()), "AirLink"), pa_context_unref);
    if (!context || pa_context_connect(context.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
        return false;
    auto step = [&] {
        if (stop.stop_requested() || pa_mainloop_iterate(loop.get(), 0, nullptr) < 0) return false;
        std::this_thread::sleep_for(5ms);
        return PA_CONTEXT_IS_GOOD(pa_context_get_state(context.get())) != 0;
    };
    while (pa_context_get_state(context.get()) != PA_CONTEXT_READY)
        if (!step()) return false;

    PulseScreenOutput route(loop.get(), context.get());
    if (!route.create() || stop.stop_requested()) return false;
    const pa_sample_spec spec{PA_SAMPLE_S16LE, 48000, 1};
    std::unique_ptr<pa_stream, decltype(&pa_stream_unref)> stream(
        pa_stream_new(context.get(), "Ekran paylaşımı sesi", &spec, nullptr), pa_stream_unref);
    if (!stream) return false;
    pa_buffer_attr buffer{uint32_t(-1), uint32_t(-1), uint32_t(-1), uint32_t(-1), 4800};
    const auto monitor = route.monitor();
    const auto flags = pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
    if (pa_stream_connect_record(stream.get(), monitor.c_str(), &buffer, flags) < 0) return false;

    PacketWriter writer;
    auto nextRedirect = std::chrono::steady_clock::time_point{};
    while (step()) {
        const auto state = pa_stream_get_state(stream.get());
        if (!PA_STREAM_IS_GOOD(state)) return false;
        if (state != PA_STREAM_READY) continue;
        if (std::chrono::steady_clock::now() >= nextRedirect) {
            if (!route.redirect()) return false;
            nextRedirect = std::chrono::steady_clock::now() + 100ms;
        }
        size_t readable = pa_stream_readable_size(stream.get());
        if (readable == size_t(-1)) return false;
        while (readable > 0 && !stop.stop_requested()) {
            const void* data = nullptr;
            size_t size = 0;
            if (pa_stream_peek(stream.get(), &data, &size) < 0) return false;
            if (size == 0) break;
            writer.append(static_cast<const uint8_t*>(data), size);
            if (pa_stream_drop(stream.get()) < 0) return false;
            readable -= std::min(readable, size);
        }
    }
    return stop.stop_requested();
}

#endif
}  // namespace

void startDesktopAudioCapture() {
    stopDesktopAudioCapture();
    auto& state = capture();
    state.worker = std::jthread([](std::stop_token stop) {
        if (!runCapture(stop) && !stop.stop_requested()) capture().failed = true;
    });
}

void stopDesktopAudioCapture() {
    auto& state = capture();
    if (state.worker.joinable()) {
        state.worker.request_stop();
        state.worker.join();
    }
    std::lock_guard lock(state.mutex);
    state.latest.clear();
    state.failed = false;
}

std::vector<uint8_t> takeDesktopAudio() {
    auto& state = capture();
    std::lock_guard lock(state.mutex);
    std::vector<uint8_t> pcm;
    pcm.swap(state.latest);
    return pcm;
}

bool desktopAudioCaptureFailed() { return capture().failed; }

}  // namespace airlink::platform
