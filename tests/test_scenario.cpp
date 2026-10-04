// Scenario check with real audio:
//   sine -> real sink;  loopback (real sink monitor -> null sink);
//   second engine listens to the null sink monitor and reports levels.
// Build: cmake --build build --target test_scenario
#include "pipewire_engine.hpp"

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

namespace {

PipeWireEngine::Device findSink(const std::vector<PipeWireEngine::Device> &devices,
                                bool (*match)(const PipeWireEngine::Device &)) {
    for (const auto &d : devices) {
        if (d.isSink && match(d)) {
            return d;
        }
    }
    return {};
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    PipeWireEngine loopEngine;
    PipeWireEngine probeEngine;
    if (!loopEngine.ok() || !probeEngine.ok()) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    // 1. null sink to loop into (drop stale ones from earlier runs first)
    QProcess::execute(QStringLiteral("pactl"),
                      {QStringLiteral("unload-module"), QStringLiteral("module-null-sink")});
    QProcess::execute(QStringLiteral("pactl"),
                      {QStringLiteral("load-module"), QStringLiteral("module-null-sink"),
                       QStringLiteral("sink_name=loopbacker_test")});

    // 2. sine wav
    QProcess::execute(
        QStringLiteral("python3"),
        {QStringLiteral("-c"),
         QStringLiteral("import wave,math,struct\n"
                        "w=wave.open('/tmp/lb_sine.wav','w')\n"
                        "w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)\n"
                        "w.writeframes(b''.join(struct.pack('<hh', "
                        "int(12000*math.sin(2*math.pi*440*i/48000)), "
                        "int(12000*math.sin(2*math.pi*440*i/48000))) for i in range(48000*10)))\n"
                        "w.close()")});

    int ticks = 0;
    QTimer picker;
    picker.setInterval(500);
    QObject::connect(&picker, &QTimer::timeout, &picker, [&] {
        const auto devices = loopEngine.devices();
        const auto realSink = findSink(devices, [](const PipeWireEngine::Device &d) {
            return !d.name.contains(QStringLiteral("loopbacker_test"));
        });
        // fresh null sink = highest serial among the matches
        PipeWireEngine::Device nullSink;
        for (const auto &d : devices) {
            if (d.isSink && d.name.contains(QStringLiteral("loopbacker_test")) &&
                d.serial > nullSink.serial) {
                nullSink = d;
            }
        }
        if (realSink.serial == 0 || nullSink.serial == 0) {
            if (++ticks > 20) {
                for (const auto &d : devices) {
                    printf("device: '%s' sink=%d serial=%llu\n", qPrintable(d.name), d.isSink,
                           (unsigned long long)d.serial);
                }
                app.exit(2);
            }
            return;
        }
        picker.stop();

        printf("loop: '%s' (monitor) -> '%s'\n", qPrintable(realSink.name),
               qPrintable(nullSink.name));
        loopEngine.setVolume(1.0f);
        probeEngine.setVolume(0.0f);
        if (!loopEngine.enable(realSink.serial, true, nullSink.serial) ||
            !probeEngine.enable(nullSink.serial, true, realSink.serial)) {
            fprintf(stderr, "enable failed\n");
            app.exit(3);
            return;
        }

        // 3. play sine into the real sink
        QProcess detached;
        detached.setProgram(QStringLiteral("pw-cat"));
        detached.setArguments({QStringLiteral("--playback"), QStringLiteral("--target"),
                               QString::number(realSink.serial), QStringLiteral("--rate"),
                               QStringLiteral("48000"), QStringLiteral("--channels"),
                               QStringLiteral("2"), QStringLiteral("/tmp/lb_sine.wav")});
        detached.startDetached();
    });

    QTimer report;
    report.setInterval(1000);
    int reportCount = 0;
    QObject::connect(&report, &QTimer::timeout, &report, [&] {
        const auto d = loopEngine.diag();
        const auto p = probeEngine.diag();
        printf("loop[cap=%llu pb=%llu w=%llu r=%llu] probe[cap=%llu lvl=%.3f/%.3f]\n",
               (unsigned long long)d.captureCallbacks, (unsigned long long)d.playbackCallbacks,
               (unsigned long long)d.ringWrittenBytes, (unsigned long long)d.ringReadBytes,
               (unsigned long long)p.captureCallbacks, probeEngine.leftLevel(),
               probeEngine.rightLevel());
        if (++reportCount == 4) {
            // probe meters > 0 means audio actually reached the null sink
            const bool audioFlows = probeEngine.leftLevel() > 0.01f;
            printf("%s\n", audioFlows ? "AUDIO FLOWS" : "NO AUDIO THROUGH LOOPBACK");
            QProcess::execute(QStringLiteral("pactl"), {QStringLiteral("unload-module"),
                                                        QStringLiteral("module-null-sink")});
            app.exit(audioFlows ? 0 : 1);
        }
    });

    QTimer::singleShot(30000, [&] { app.exit(5); });
    picker.start();
    report.start();
    return app.exec();
}