#include "pipewire_engine.hpp"

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/dict.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

// fixed F32/stereo/48k — adapters resample & channelmix for us.
// Upgrade path: negotiate the capture format and mirror it on playback.
constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kSampleBytes = sizeof(float);            // planar: one float per frame per plane
constexpr uint32_t kRingFrames = 8192;                      // ~170 ms of headroom
constexpr uint32_t kRingBytes = kRingFrames * kSampleBytes; // per channel

// unique per process so concurrent engines never share a scheduling group
std::string nodeGroupName() {
    static const std::string name = "pw-looper-" + std::to_string(::getpid());
    return name;
}

void handleState(PipeWireEngine *self, enum pw_stream_state newState, const char *error) {
    if (newState == PW_STREAM_STATE_ERROR) {
        emit self->error(QString::fromUtf8(error != nullptr ? error : "unknown stream error"));
    } else {
        emit self->stateChanged(QString::fromUtf8(pw_stream_state_as_string(newState)));
    }
}

// Builds the EnumFormat param both streams offer. The pod lives in `buf`, which
// must outlive the pw_stream_connect() calls that consume it.
// F32P on purpose — the DSP-native planar format, adapters hand it
// through with zero conversion; the RT path below only handles planar.
void buildFormatParams(const struct spa_pod *params[1], uint8_t *buf, uint32_t bufSize) {
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, bufSize);
    struct spa_audio_info_raw raw =
        SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32P, .flags = 0u, .rate = kRate,
                                .channels = kChannels,
                                .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);
}

// Builds stream props shared by capture/playback.
struct pw_properties *makeStreamProps(const char *mediaClass, const char *nodeName,
                                      uint64_t targetSerial, bool targetIsSink) {
    auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS, mediaClass,
                                    PW_KEY_APP_NAME, "pw-looper", PW_KEY_NODE_GROUP,
                                    nodeGroupName().c_str(), PW_KEY_NODE_NAME, nodeName, NULL);
    pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%llu", (unsigned long long)targetSerial);
    if (targetIsSink) {
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    }
    return props;
}

} // namespace

const struct pw_stream_events PipeWireEngine::captureStreamEvents_ = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = &PipeWireEngine::onCaptureState,
    .process = &PipeWireEngine::onProcessCapture,
};

const struct pw_stream_events PipeWireEngine::playbackStreamEvents_ = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = &PipeWireEngine::onPlaybackState,
    .process = &PipeWireEngine::onProcessPlayback,
};

const struct pw_registry_events PipeWireEngine::registryEvents_ = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = &PipeWireEngine::onGlobal,
    .global_remove = &PipeWireEngine::onGlobalRemove,
};

PipeWireEngine::PipeWireEngine(QObject *parent) : QObject(parent) {
    ringLStorage_.assign(kRingBytes / sizeof(float), 0.0f);
    ringRStorage_.assign(kRingBytes / sizeof(float), 0.0f);

    pw_init(nullptr, nullptr);

    loop_ = pw_thread_loop_new("pw-looper", nullptr);
    if (loop_ == nullptr) {
        initError_ = "pw_thread_loop_new failed";
        return;
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
        initError_ = "pw_context_new failed";
        return;
    }

    pw_thread_loop_start(loop_);
    pw_thread_loop_lock(loop_);
    core_ = pw_context_connect(context_, nullptr, 0);
    if (core_ == nullptr) {
        initError_ = "cannot connect to PipeWire daemon";
        pw_thread_loop_unlock(loop_);
        return;
    }
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(registry_, &registryListener_, &registryEvents_, this);
    pw_thread_loop_unlock(loop_);
}

PipeWireEngine::~PipeWireEngine() {
    if (loop_ != nullptr) {
        // stop dispatching first, then tear down objects that own loop sources;
        // the thread loop (and its pw_loop) must die last
        pw_thread_loop_stop(loop_);
        destroyStreams();
        if (core_ != nullptr) {
            spa_hook_remove(&registryListener_);
            pw_core_disconnect(core_);
            core_ = nullptr;
        }
        registry_ = nullptr;
        if (context_ != nullptr) {
            pw_context_destroy(context_);
            context_ = nullptr;
        }
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    pw_deinit();
}

std::vector<PipeWireEngine::Device> PipeWireEngine::devices() const {
    const std::scoped_lock lock(devicesMutex_);
    std::vector<Device> out;
    out.reserve(devicesById_.size());
    for (const auto &[id, dev] : devicesById_) {
        out.push_back(dev);
    }
    return out;
}

PipeWireEngine::Diag PipeWireEngine::diag() const {
    Diag d;
    d.captureCallbacks = captureCallbacks_.load(std::memory_order_relaxed);
    d.playbackCallbacks = playbackCallbacks_.load(std::memory_order_relaxed);
    d.ringWrittenBytes = ringWrittenBytes_.load(std::memory_order_relaxed);
    d.ringReadBytes = ringReadBytes_.load(std::memory_order_relaxed);
    return d;
}

void PipeWireEngine::onGlobal(void *data, uint32_t id, uint32_t /*permissions*/, const char *type,
                              uint32_t /*version*/, const struct spa_dict *props) {
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == nullptr) {
        return;
    }
    const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (cls == nullptr) {
        return;
    }
    const bool isSink = std::strcmp(cls, "Audio/Sink") == 0;
    const bool isSource = std::strcmp(cls, "Audio/Source") == 0;
    if (!isSink && !isSource) {
        return;
    }
    const char *app = spa_dict_lookup(props, PW_KEY_APP_NAME);
    if (app != nullptr && std::strcmp(app, "pw-looper") == 0) {
        return; // don't list our own streams
    }

    Device dev;
    dev.globalId = id;
    dev.isSink = isSink;
    if (const char *s = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL); s != nullptr) {
        dev.serial = std::strtoull(s, nullptr, 0);
    }
    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (desc == nullptr) {
        desc = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        desc = desc != nullptr ? desc : "?";
    }
    dev.name = QString::fromUtf8(desc);

    auto *self = static_cast<PipeWireEngine *>(data);
    {
        const std::scoped_lock lock(self->devicesMutex_);
        self->devicesById_[id] = dev;
    }
    emit self->devicesChanged();
}

void PipeWireEngine::onGlobalRemove(void *data, uint32_t id) {
    auto *self = static_cast<PipeWireEngine *>(data);
    {
        const std::scoped_lock lock(self->devicesMutex_);
        self->devicesById_.erase(id);
    }
    emit self->devicesChanged();
}

void PipeWireEngine::destroyStreams() {
    if (playback_ != nullptr) {
        pw_stream_destroy(playback_);
        playback_ = nullptr;
    }
    if (capture_ != nullptr) {
        pw_stream_destroy(capture_);
        capture_ = nullptr;
    }
    levelL_.store(0.0f, std::memory_order_relaxed);
    levelR_.store(0.0f, std::memory_order_relaxed);
}

bool PipeWireEngine::enable(uint64_t captureSerial, bool captureIsSink, uint64_t playbackSerial) {
    if (!ok()) {
        return false;
    }

    pw_thread_loop_lock(loop_);
    destroyStreams();

    auto *loop = pw_thread_loop_get_loop(loop_);
    uint8_t formatBuf[1024];
    const struct spa_pod *params[1];
    buildFormatParams(params, formatBuf, sizeof(formatBuf));

    capture_ = pw_stream_new_simple(
        loop, "pw-looper-capture",
        makeStreamProps("Stream/Input/Audio", "pw-looper-capture", captureSerial, captureIsSink),
        &captureStreamEvents_, this);
    playback_ = pw_stream_new_simple(
        loop, "pw-looper-playback",
        makeStreamProps("Stream/Output/Audio", "pw-looper-playback", playbackSerial, false),
        &playbackStreamEvents_, this);
    if (capture_ == nullptr || playback_ == nullptr) {
        destroyStreams();
        pw_thread_loop_unlock(loop_);
        emit error("failed to create streams");
        return false;
    }

    const auto flags = static_cast<enum pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(capture_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0 ||
        pw_stream_connect(playback_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        destroyStreams();
        pw_thread_loop_unlock(loop_);
        emit error("failed to connect streams");
        return false;
    }

    // set volume once after connect; slider changes re-apply it.
    if (pw_stream_set_control(playback_, SPA_PROP_volume, 1, &volume_, 0) < 0) {
        pw_log_warn("volume control not available yet");
    }

    pw_thread_loop_unlock(loop_);
    return true;
}

void PipeWireEngine::disable() {
    if (!ok()) {
        return;
    }
    // Stream teardown touches loop sources and the streams' data-loop threads,
    // so run it on the pipeWire loop thread (blocks until done).
    pw_loop_invoke(pw_thread_loop_get_loop(loop_), &destroyStreamsOnLoop, 0, nullptr, 0, true,
                   this);
}

int PipeWireEngine::destroyStreamsOnLoop(struct spa_loop * /*loop*/, bool /*async*/,
                                         uint32_t /*seq*/, const void * /*data*/, size_t /*size*/,
                                         void *user_data) {
    static_cast<PipeWireEngine *>(user_data)->destroyStreams();
    return 0;
}

void PipeWireEngine::setVolume(float volume) {
    if (!ok()) {
        return;
    }
    pw_thread_loop_lock(loop_);
    volume_ = std::max(volume, 0.0f); // no upper clamp — override >100% allowed
    if (playback_ != nullptr) {
        pw_stream_set_control(playback_, SPA_PROP_volume, 1, &volume_, 0);
    }
    pw_thread_loop_unlock(loop_);
}

void PipeWireEngine::onCaptureState(void *data, enum pw_stream_state /*oldState*/,
                                    enum pw_stream_state newState, const char *error) {
    handleState(static_cast<PipeWireEngine *>(data), newState, error);
}

void PipeWireEngine::onPlaybackState(void *data, enum pw_stream_state /*oldState*/,
                                     enum pw_stream_state newState, const char *error) {
    auto *self = static_cast<PipeWireEngine *>(data);
    handleState(self, newState, error);
    // The adapter (and its volume control) only exists once the stream is set up;
    // we run on the loop thread here, so setting the control is safe.
    if (newState == PW_STREAM_STATE_PAUSED || newState == PW_STREAM_STATE_STREAMING) {
        pw_stream_set_control(self->playback_, SPA_PROP_volume, 1, &self->volume_, 0);
    }
}

// RT thread: measure peaks per plane and feed the shared per-channel rings.
void PipeWireEngine::onProcessCapture(void *data) {
    auto *self = static_cast<PipeWireEngine *>(data);
    struct pw_buffer *b = pw_stream_dequeue_buffer(self->capture_);
    if (b == nullptr) {
        return;
    }
    struct spa_buffer *sb = b->buffer;
    self->captureCallbacks_.fetch_add(1, std::memory_order_relaxed);

    if (sb->n_datas >= 2 && sb->datas[0].data != nullptr && sb->datas[1].data != nullptr) {
        const auto *plane0 = reinterpret_cast<const float *>(
            static_cast<const char *>(sb->datas[0].data) + sb->datas[0].chunk->offset);
        const auto *plane1 = reinterpret_cast<const float *>(
            static_cast<const char *>(sb->datas[1].data) + sb->datas[1].chunk->offset);
        const uint32_t frames =
            std::min<uint32_t>(sb->datas[0].chunk->size, sb->datas[1].chunk->size) / kSampleBytes;

        float peakL = 0.0f;
        float peakR = 0.0f;
        for (uint32_t f = 0; f < frames; ++f) {
            peakL = std::max(peakL, std::fabs(plane0[f]));
            peakR = std::max(peakR, std::fabs(plane1[f]));
        }
        self->levelL_.store(peakL, std::memory_order_relaxed);
        self->levelR_.store(peakR, std::memory_order_relaxed);

        const uint32_t bytes = std::min<uint64_t>(frames * kSampleBytes, kRingBytes);
        uint32_t wl = 0;
        uint32_t wr = 0;
        const int32_t usedL = spa_ringbuffer_get_write_index(&self->ringL_, &wl);
        const int32_t usedR = spa_ringbuffer_get_write_index(&self->ringR_, &wr);
        if (usedL >= 0 && usedR >= 0 && usedL + bytes <= kRingBytes) {
            spa_ringbuffer_write_data(&self->ringL_, self->ringLStorage_.data(), kRingBytes,
                                      wl % kRingBytes, plane0, bytes);
            spa_ringbuffer_write_update(&self->ringL_, wl + bytes);
            spa_ringbuffer_write_data(&self->ringR_, self->ringRStorage_.data(), kRingBytes,
                                      wr % kRingBytes, plane1, bytes);
            spa_ringbuffer_write_update(&self->ringR_, wr + bytes);
            self->ringWrittenBytes_.fetch_add(2 * bytes, std::memory_order_relaxed);
        }
        // ring full -> drop this quantum; only happens if playback stalls.
    }
    pw_stream_queue_buffer(self->capture_, b);
}

// RT thread: pull from the per-channel rings, zero-fill planes on underrun.
void PipeWireEngine::onProcessPlayback(void *data) {
    auto *self = static_cast<PipeWireEngine *>(data);
    struct pw_buffer *b = pw_stream_dequeue_buffer(self->playback_);
    if (b == nullptr) {
        return;
    }
    struct spa_buffer *sb = b->buffer;
    self->playbackCallbacks_.fetch_add(1, std::memory_order_relaxed);

    if (sb->n_datas >= 2 && sb->datas[0].data != nullptr && sb->datas[1].data != nullptr) {
        uint32_t frames = sb->datas[0].maxsize / kSampleBytes;
        if (b->requested > 0) {
            frames = std::min<uint64_t>(frames, b->requested);
        }
        const uint32_t bytes = frames * kSampleBytes;

        uint32_t rl = 0;
        uint32_t rr = 0;
        const int32_t availL = spa_ringbuffer_get_read_index(&self->ringL_, &rl);
        const int32_t availR = spa_ringbuffer_get_read_index(&self->ringR_, &rr);
        const uint32_t gotL =
            availL > 0 ? std::min<uint32_t>(static_cast<uint32_t>(availL), bytes) : 0;
        const uint32_t gotR =
            availR > 0 ? std::min<uint32_t>(static_cast<uint32_t>(availR), bytes) : 0;

        if (gotL > 0) {
            spa_ringbuffer_read_data(&self->ringL_, self->ringLStorage_.data(), kRingBytes,
                                     rl % kRingBytes, sb->datas[0].data, gotL);
            spa_ringbuffer_read_update(&self->ringL_, rl + gotL);
            self->ringReadBytes_.fetch_add(gotL, std::memory_order_relaxed);
        }
        if (gotR > 0) {
            spa_ringbuffer_read_data(&self->ringR_, self->ringRStorage_.data(), kRingBytes,
                                     rr % kRingBytes, sb->datas[1].data, gotR);
            spa_ringbuffer_read_update(&self->ringR_, rr + gotR);
        }
        if (gotL < bytes) {
            std::memset(static_cast<char *>(sb->datas[0].data) + gotL, 0, bytes - gotL);
        }
        if (gotR < bytes) {
            std::memset(static_cast<char *>(sb->datas[1].data) + gotR, 0, bytes - gotR);
        }

        for (int i = 0; i < 2; ++i) {
            sb->datas[i].chunk->offset = 0;
            sb->datas[i].chunk->size = bytes;
            sb->datas[i].chunk->stride = kSampleBytes;
        }
    }
    pw_stream_queue_buffer(self->playback_, b);
}