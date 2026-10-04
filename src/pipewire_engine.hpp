#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include <spa/utils/hook.h>
#include <spa/utils/ringbuffer.h>

#include <pipewire/stream.h>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_stream;
struct spa_dict;
struct spa_loop;

// Owns the PipeWire thread loop and the loopback streams. Never touches Qt UI:
// state flows out through signals, control flows in through methods.
class PipeWireEngine : public QObject {
    Q_OBJECT
  public:
    struct Device {
        uint64_t serial = 0;
        uint32_t globalId = 0;
        QString name;
        bool isSink = false; // sinks double as monitor input
    };

    explicit PipeWireEngine(QObject *parent = nullptr);
    ~PipeWireEngine() override;

    PipeWireEngine(const PipeWireEngine &) = delete;
    PipeWireEngine &operator=(const PipeWireEngine &) = delete;

    [[nodiscard]] bool ok() const { return initError_.isEmpty(); }
    [[nodiscard]] const QString &errorString() const { return initError_; }
    [[nodiscard]] std::vector<Device> devices() const;
    [[nodiscard]] float leftLevel() const { return levelL_.load(std::memory_order_relaxed); }
    [[nodiscard]] float rightLevel() const { return levelR_.load(std::memory_order_relaxed); }

    struct Diag {
        uint64_t captureCallbacks = 0;
        uint64_t playbackCallbacks = 0;
        uint64_t ringWrittenBytes = 0;
        uint64_t ringReadBytes = 0;
    };
    [[nodiscard]] Diag diag() const;

    bool enable(uint64_t captureSerial, bool captureIsSink, uint64_t playbackSerial);
    void disable();
    void setVolume(float volume); // ≥ 0.0 (1.0 = 100%)

  signals:
    void devicesChanged();
    void stateChanged(const QString &status);
    void error(const QString &message);

  private:
    static void onGlobal(void *data, uint32_t id, uint32_t permissions, const char *type,
                         uint32_t version, const struct spa_dict *props);
    static void onGlobalRemove(void *data, uint32_t id);
    static void onCaptureState(void *data, enum pw_stream_state oldState,
                               enum pw_stream_state newState, const char *error);
    static void onPlaybackState(void *data, enum pw_stream_state oldState,
                                enum pw_stream_state newState, const char *error);
    static void onProcessCapture(void *data);
    static void onProcessPlayback(void *data);
    static int destroyStreamsOnLoop(struct spa_loop *loop, bool async, uint32_t seq,
                                    const void *data, size_t size, void *user_data);

    void destroyStreams();

    QString initError_;

    static const struct pw_stream_events captureStreamEvents_;
    static const struct pw_stream_events playbackStreamEvents_;
    static const struct pw_registry_events registryEvents_;

    mutable std::mutex devicesMutex_;
    std::map<uint32_t, Device> devicesById_;

    struct pw_thread_loop *loop_ = nullptr;
    struct pw_context *context_ = nullptr;
    struct pw_core *core_ = nullptr;
    struct pw_registry *registry_ = nullptr;
    struct spa_hook registryListener_ = {};
    struct spa_hook captureListener_ = {};
    struct spa_hook playbackListener_ = {};

    struct pw_stream *capture_ = nullptr;
    struct pw_stream *playback_ = nullptr;

    struct spa_ringbuffer ringL_ = {};
    struct spa_ringbuffer ringR_ = {};
    std::vector<float> ringLStorage_; // planar: one ring per channel
    std::vector<float> ringRStorage_;

    std::atomic<float> levelL_ = 0.0f;
    std::atomic<float> levelR_ = 0.0f;
    // RT-path diagnostics: where does the data stop?
    std::atomic<uint64_t> captureCallbacks_ = 0;
    std::atomic<uint64_t> playbackCallbacks_ = 0;
    std::atomic<uint64_t> ringWrittenBytes_ = 0;
    std::atomic<uint64_t> ringReadBytes_ = 0;
    float volume_ = 1.0f; // guarded by the thread loop lock
};