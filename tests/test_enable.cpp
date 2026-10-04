// integration check — enable() must put BOTH streams into STREAMING.
// Needs a running PipeWire session; run headless: ./build/test_loopback_enable
#include "pipewire_engine.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    PipeWireEngine engine;
    if (!engine.ok()) {
        fprintf(stderr, "init failed: %s\n", engine.errorString().toUtf8().constData());
        return 1;
    }

    int streamingCount = 0;
    int unconnectedCount = 0;
    bool failed = false;
    QObject::connect(&engine, &PipeWireEngine::stateChanged, &engine, [&](const QString &s) {
        printf("state: %s\n", qPrintable(s));
        if (s == QStringLiteral("streaming")) {
            if (++streamingCount == 2) {
                printf("both streams reached STREAMING — disabling from Qt thread\n");
                QTimer::singleShot(1500, [&] { engine.disable(); });
            }
        }
        if (s == QStringLiteral("unconnected")) {
            if (++unconnectedCount == 2) {
                printf("both streams unconnected after disable — OK\n");
                app.exit(0);
            }
        }
    });
    QObject::connect(&engine, &PipeWireEngine::error, &engine, [&](const QString &s) {
        fprintf(stderr, "ERROR: %s\n", qPrintable(s));
        failed = true;
    });

    int tries = 0;
    QTimer picker;
    picker.setInterval(500);
    QObject::connect(&picker, &QTimer::timeout, &picker, [&] {
        const auto devices = engine.devices();
        const PipeWireEngine::Device *in = nullptr;
        const PipeWireEngine::Device *out = nullptr;
        for (const auto &d : devices) {
            if (d.isSink && out == nullptr)
                out = &d;
            else if (!d.isSink && in == nullptr)
                in = &d;
        }
        if (out == nullptr) {
            if (++tries > 20) {
                fprintf(stderr, "no sink found\n");
                app.exit(2);
            }
            return;
        }
        if (in == nullptr && out != nullptr) {
            // single-sink box -> loop the sink into itself (self-capture)
            in = out;
        }
        picker.stop();
        printf("enabling: %s -> %s\n", qPrintable(in->name), qPrintable(out->name));
        engine.setVolume(0.0f); // avoid feedback during the check
        if (!engine.enable(in->serial, in->isSink, out->serial)) {
            fprintf(stderr, "enable() returned false\n");
            app.exit(3);
        }
    });
    picker.start();

    QTimer::singleShot(15000, [&] {
        if (!failed) {
            fprintf(stderr, "timeout waiting for state transitions\n");
        }
        app.exit(failed ? 1 : 4);
    });

    return app.exec();
}