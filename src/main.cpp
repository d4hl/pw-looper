#include "main_window.hpp"

#include <QApplication>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("pw-looper"));
    QApplication::setApplicationDisplayName(QStringLiteral("pw-looper"));
    QApplication::setOrganizationName(QStringLiteral("pw-looper"));
    QApplication::setQuitOnLastWindowClosed(false); // window close = hide, tray keeps running

    // ponytail: single instance via lockfile; second launch pokes the first
    // over a local socket to raise its window, no shared library dep needed
    QLockFile lock(
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
        "/pw-looper.lock");
    lock.setStaleLockTime(0);

    QLocalServer server;
    if (lock.tryLock(0)) {
        // stale socket from a crashed run
        QLocalServer::removeServer(QStringLiteral("pw-looper"));
        server.listen(QStringLiteral("pw-looper"));
    } else {
        QLocalSocket sock;
        sock.connectToServer(QStringLiteral("pw-looper"));
        if (sock.waitForConnected(1000)) {
            sock.write("show");
            sock.flush();
            sock.waitForBytesWritten(1000);
            sock.disconnectFromServer();
        }
        return 0;
    }

    MainWindow window;
    window.show();

    QObject::connect(&server, &QLocalServer::newConnection, [&window, &server] {
        while (server.hasPendingConnections()) {
            server.nextPendingConnection()->deleteLater();
            window.showNormal();
            window.raise();
            window.activateWindow();
        }
    });

    return QApplication::exec();
}
