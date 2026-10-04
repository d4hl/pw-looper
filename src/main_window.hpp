#pragma once

#include <QElapsedTimer>
#include <QSystemTrayIcon>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QTimer;

class PipeWireEngine;

class MainWindow : public QWidget {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget *parent = nullptr);

  protected:
    void closeEvent(QCloseEvent *event) override; // close = hide, app stays in tray

  private:
    void rebuildDevices();
    void updateToggle();
    void onToggleClicked();
    void onDeviceChanged();
    void refreshLevels();

    PipeWireEngine *engine_;
    QComboBox *inputBox_;
    QComboBox *outputBox_;
    QSlider *volumeSlider_;
    QDoubleSpinBox *volumeSpinBox_;
    QProgressBar *levelL_;
    QProgressBar *levelR_;
    QPushButton *toggleButton_;
    QLabel *timerLabel_;
    QSystemTrayIcon *tray_;
    QTimer *meterTimer_;
    QAction *toggleAction_;
    bool running_ = false;
    float shownL_ = 0.0f;
    float shownR_ = 0.0f;
    bool skipDeviceChange_ = false;
    QElapsedTimer elapsedTimer_;
};