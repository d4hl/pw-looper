#include "main_window.hpp"

#include <QApplication>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("pw-looper"));
    QApplication::setApplicationDisplayName(QStringLiteral("pw-looper"));
    QApplication::setOrganizationName(QStringLiteral("pw-looper"));
    QApplication::setQuitOnLastWindowClosed(false); // window close = hide, tray keeps running

    MainWindow window;
    window.show();

    return QApplication::exec();
}