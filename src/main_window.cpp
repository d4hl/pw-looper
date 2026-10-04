#include "main_window.hpp"

#include "pipewire_engine.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>

namespace {
constexpr int kMeterIntervalMs = 50;
constexpr float kDecay = 0.85f;
constexpr auto kSerialRole = Qt::UserRole;     // qulonglong object serial
constexpr auto kIsSinkRole = Qt::UserRole + 1; // bool
} // namespace

MainWindow::MainWindow(QWidget *parent) : QWidget(parent) {
    setWindowTitle(tr("pw-looper"));

    engine_ = new PipeWireEngine(this);
    connect(engine_, &PipeWireEngine::devicesChanged, this, &MainWindow::rebuildDevices);
    connect(engine_, &PipeWireEngine::stateChanged, this,
            [this](const QString &status) {
                toggleButton_->setToolTip(status);
            });
    connect(engine_, &PipeWireEngine::error, this,
            [this](const QString &msg) {
                toggleButton_->setText(tr("Error"));
                toggleButton_->setToolTip(msg);
            });

    // ── Top row: button + stretch + timer ─────────────────────────────
    toggleButton_ = new QPushButton(tr("Enable"));
    toggleButton_->setToolTip(tr("idle"));
    timerLabel_ = new QLabel(QStringLiteral("00:00"));

    auto *topRow = new QHBoxLayout;
    topRow->addWidget(toggleButton_);
    topRow->addStretch(1);
    topRow->addWidget(timerLabel_);

    // ── Monitor: levels ──────────────────────────────────────────────────
    auto *lvlRow = new QHBoxLayout;
    auto makeBar = [] {
        auto *bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setTextVisible(false);
        bar->setFixedHeight(14);
        bar->setMinimumSize(60, 14);  // narrow bars
        return bar;
    };
    levelL_ = makeBar();
    levelR_ = makeBar();
    lvlRow->addWidget(new QLabel(QStringLiteral("L")));
    lvlRow->addWidget(levelL_);
    lvlRow->addWidget(new QLabel(QStringLiteral("R")));
    lvlRow->addWidget(levelR_);

    // ── Settings ─────────────────────────────────────────────────────────
    auto *settings = new QVBoxLayout;
    settings->setContentsMargins(8, 8, 8, 8);

    // Device: Mon [▽] → Redir [▽]
    auto *devRow = new QHBoxLayout;
    inputBox_ = new QComboBox;
    inputBox_->setMinimumSize(80, 0);
    outputBox_ = new QComboBox;
    outputBox_->setMinimumSize(80, 0);
    devRow->addWidget(new QLabel(tr("Monitor")));
    devRow->addWidget(inputBox_);
    devRow->addWidget(new QLabel(QStringLiteral("→")));
    devRow->addWidget(new QLabel(tr("Destination")));
    devRow->addWidget(outputBox_);
    devRow->addStretch(1);
    settings->addLayout(devRow);

    // Volume: [slider] [150% ▾]
    auto *volRow = new QHBoxLayout;
    volumeSlider_ = new QSlider(Qt::Horizontal);
    volumeSlider_->setRange(0, 200);
    volumeSlider_->setValue(100);
    volumeSlider_->setMinimumSize(80, 0);
    volumeSpinBox_ = new QDoubleSpinBox;
    volumeSpinBox_->setRange(0.0, 200.0);
    volumeSpinBox_->setValue(100.0);
    volumeSpinBox_->setSuffix(QStringLiteral("%"));
    volumeSpinBox_->setDecimals(0);
    volumeSpinBox_->setSingleStep(1.0);
    volumeSpinBox_->setMinimumSize(50, 0);
    volRow->addWidget(volumeSlider_, 1);
    volRow->addWidget(volumeSpinBox_);
    settings->addLayout(volRow);

    // ── Main layout ──────────────────────────────────────────────────────
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);
    layout->addLayout(topRow);
    layout->addLayout(lvlRow);
    layout->addLayout(settings);
    layout->addStretch(1);  // free space at bottom

    // ── Timer & meter ────────────────────────────────────────────────────
    meterTimer_ = new QTimer(this);
    meterTimer_->setInterval(kMeterIntervalMs);
    connect(meterTimer_, &QTimer::timeout, this, &MainWindow::refreshLevels);
    meterTimer_->start();

    // ── Auto-restart on device change ───────────────────────────────────
    connect(inputBox_, &QComboBox::currentIndexChanged, this,
            &MainWindow::onDeviceChanged);
    connect(outputBox_, &QComboBox::currentIndexChanged, this,
            &MainWindow::onDeviceChanged);

    // ── Signals ──────────────────────────────────────────────────────────
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        volumeSpinBox_->setValue(static_cast<double>(value));
        engine_->setVolume(value / 100.0f);
    });
    connect(volumeSpinBox_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        volumeSlider_->setValue(qRound(value));
        engine_->setVolume(static_cast<float>(value / 100.0));
    });
    connect(toggleButton_, &QPushButton::clicked, this, &MainWindow::onToggleClicked);

    // ── Tray ─────────────────────────────────────────────────────────────
    tray_ = new QSystemTrayIcon(this);
    QIcon icon = QIcon::fromTheme(QStringLiteral("audio-card"));
    if (icon.isNull()) {
        icon = style()->standardIcon(QStyle::SP_MediaVolume);
    }
    tray_->setIcon(icon);
    tray_->setToolTip(tr("pw-looper"));

    auto *menu = new QMenu(this);
    auto *showAction = menu->addAction(tr("Show"), this, [this] {
        show();
        raise();
        activateWindow();
    });
    toggleAction_ = menu->addAction(QString(), this, &MainWindow::onToggleClicked);
    menu->addSeparator();
    menu->addAction(tr("Quit"), qApp, &QApplication::quit);
    tray_->setContextMenu(menu);
    connect(tray_, &QSystemTrayIcon::activated, this, [this, showAction](auto reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            isVisible() ? hide() : showAction->trigger();
        }
    });

    updateToggle();
    rebuildDevices();

    if (!engine_->ok()) {
        toggleButton_->setText(tr("Error"));
        toggleButton_->setToolTip(engine_->errorString());
        toggleButton_->setEnabled(false);
        QMessageBox::warning(this, tr("pw-looper"),
                             tr("Cannot connect to PipeWire: %1").arg(engine_->errorString()));
    } else {
        tray_->show();
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    hide();
    event->ignore();
}

void MainWindow::rebuildDevices() {
    skipDeviceChange_ = true;

    const auto devices = engine_->devices();
    bool inStillValid = false;
    bool outStillValid = false;

    const auto refill = [devices](QComboBox *box, bool sinksOnly, bool &stillValid) {
        const auto prevSerial = box->currentData(kSerialRole).value<uint64_t>();
        box->clear();
        for (const auto &dev : devices) {
            if (sinksOnly && !dev.isSink) {
                continue;
            }
            const QString label = dev.isSink ? tr("%1 (monitor)").arg(dev.name) : dev.name;
            box->addItem(label);
            box->setItemData(box->count() - 1, QVariant::fromValue(dev.serial), kSerialRole);
            box->setItemData(box->count() - 1, dev.isSink, kIsSinkRole);
            if (dev.serial == prevSerial) {
                stillValid = true;
            }
        }
        const int idx = box->findData(QVariant::fromValue(prevSerial), kSerialRole);
        if (idx >= 0) {
            box->setCurrentIndex(idx);
        }
    };
    refill(inputBox_, false, inStillValid);  // sources + sinks (as monitors)
    refill(outputBox_, true, outStillValid);  // sinks only

    skipDeviceChange_ = false;

    // If the loopback is running but a selected device disappeared → disable
    if (running_ && (!inStillValid || !outStillValid)) {
        engine_->disable();
        running_ = false;
        elapsedTimer_.invalidate();
        updateToggle();
    }
}

void MainWindow::updateToggle() {
    if (running_) {
        toggleButton_->setText(tr("Disable"));
        toggleButton_->setToolTip(tr("Disable loopback"));
        toggleAction_->setText(tr("Disable"));
    } else {
        toggleButton_->setText(tr("Enable"));
        toggleButton_->setToolTip(tr("idle"));
        toggleAction_->setText(tr("Enable"));
        timerLabel_->setText(QStringLiteral("00:00"));
    }
}

void MainWindow::onToggleClicked() {
    if (running_) {
        engine_->disable();
        running_ = false;
        elapsedTimer_.invalidate();
    } else {
        const auto in = inputBox_->currentData(kSerialRole).value<uint64_t>();
        const bool inIsSink = inputBox_->currentData(kIsSinkRole).value<bool>();
        const auto out = outputBox_->currentData(kSerialRole).value<uint64_t>();
        if (in == 0 || out == 0) {
            toggleButton_->setToolTip(tr("select devices first"));
            return;
        }
        running_ = engine_->enable(in, inIsSink, out);
        if (running_) {
            elapsedTimer_.start();
        }
    }
    updateToggle();
}

void MainWindow::onDeviceChanged() {
    if (skipDeviceChange_) return;
    if (!running_) return;
    // Auto-restart with new device selection
    engine_->disable();
    const auto in = inputBox_->currentData(kSerialRole).value<uint64_t>();
    const bool inIsSink = inputBox_->currentData(kIsSinkRole).value<bool>();
    const auto out = outputBox_->currentData(kSerialRole).value<uint64_t>();
    if (in == 0 || out == 0) {
        running_ = false;
        elapsedTimer_.invalidate();
        updateToggle();
        return;
    }
    running_ = engine_->enable(in, inIsSink, out);
    if (running_) {
        elapsedTimer_.start();
    }
    updateToggle();
}

void MainWindow::refreshLevels() {
    const float l = running_ ? engine_->leftLevel() : 0.0f;
    const float r = running_ ? engine_->rightLevel() : 0.0f;
    shownL_ = std::max(l, shownL_ * kDecay);
    shownR_ = std::max(r, shownR_ * kDecay);
    levelL_->setValue(qRound(shownL_ * 100.0f));
    levelR_->setValue(qRound(shownR_ * 100.0f));

    if (running_ && elapsedTimer_.isValid()) {
        const qint64 ms = elapsedTimer_.elapsed();
        const int sec = static_cast<int>(ms / 1000);
        const int min = sec / 60;
        const int hr = min / 60;
        // simple format, pad9 for alignment
        if (hr > 0) {
            timerLabel_->setText(QStringLiteral("%1:%2:%3")
                .arg(hr, 2, 10, u'0')
                .arg(min % 60, 2, 10, u'0')
                .arg(sec % 60, 2, 10, u'0'));
        } else {
            timerLabel_->setText(QStringLiteral("%1:%2")
                .arg(min, 2, 10, u'0')
                .arg(sec % 60, 2, 10, u'0'));
        }
    }
}
