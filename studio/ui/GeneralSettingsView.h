#ifndef GENERAL_SETTINGS_VIEW_H
#define GENERAL_SETTINGS_VIEW_H

#include "models/AudioDeviceManager.h"   // for AudioDeviceManager
#include "models/AudioSettings.h"        // for AudioSettings
#include "models/MonitoringController.h" // for MonitoringController

#include <QCheckBox>   // for QCheckBox
#include <QComboBox>   // for QComboBox
#include <QFormLayout> // for QFormLayout
#include <QLabel>      // for QLabel
#include <QObject>     // for Q_OBJECT, slots
#include <QSlider>     // for QSlider
#include <QSpinBox>    // for QSpinBox
#include <QWidget>     // for QWidget
#include <memory>      // for shared_ptr

class GeneralSettingsView : public QWidget {
    Q_OBJECT

public:
    GeneralSettingsView(std::shared_ptr<AudioSettings> settings, std::shared_ptr<AudioDeviceManager> devices,
                        std::shared_ptr<MonitoringController> monitoring, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void refreshUi();
    void updateLatencyText();

private:
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<MonitoringController> m_monitoring;

    bool m_isRefreshing = false;

    // Processing Settings
    QFormLayout* m_procForm = nullptr;
    QComboBox* m_chunkSizeCombo = nullptr;
    QLabel* m_latencyLabel = nullptr;
    QLabel* m_latencyEquationLabel = nullptr;
    QSpinBox* m_targetLevelSpin = nullptr;
    QLabel* m_targetLevelSub = nullptr;
    QCheckBox* m_enableRateAdjustCheck = nullptr;
    QLabel* m_rateAdjustSub = nullptr;
    QWidget* m_rateAdjustIntervalRow = nullptr;
    QSlider* m_rateAdjustIntervalSlider = nullptr;
    QLabel* m_rateAdjustIntervalValLabel = nullptr;
    QSpinBox* m_queueLimitSpin = nullptr;
    QCheckBox* m_stopOnRateChangeCheck = nullptr;
    QSlider* m_measureIntervalSlider = nullptr;
    QLabel* m_measureIntervalValLabel = nullptr;
    QCheckBox* m_multithreadedCheck = nullptr;
    QSpinBox* m_workerThreadsSpin = nullptr;

    // Polling Rate
    QSlider* m_pollingRateSlider = nullptr;
    QLabel* m_pollingRateLabel = nullptr;

    // Silence Detection
    QSlider* m_silenceThresholdSlider = nullptr;
    QLabel* m_silenceThresholdLabel = nullptr;

    QSlider* m_silenceTimeoutSlider = nullptr;
    QLabel* m_silenceTimeoutLabel = nullptr;

    void setupUi();
    void applySettings();
};

#endif // GENERAL_SETTINGS_VIEW_H
