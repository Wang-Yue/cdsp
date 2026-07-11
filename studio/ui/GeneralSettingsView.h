#ifndef GENERAL_SETTINGS_VIEW_H
#define GENERAL_SETTINGS_VIEW_H

#include "models/AudioSettings.h"
#include "models/MonitoringController.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSlider>
#include <QWidget>
#include <memory>

class GeneralSettingsView : public QWidget {
    Q_OBJECT

public:
    GeneralSettingsView(std::shared_ptr<AudioSettings> settings, std::shared_ptr<MonitoringController> monitoring,
                        QWidget* parent = nullptr);

private slots:
    void refreshUi();

private:
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<MonitoringController> m_monitoring;

    QComboBox* m_themeCombo;
    QComboBox* m_logLevelCombo;
    QCheckBox* m_autoStartCheck;

    QSlider* m_pollingRateSlider;
    QLabel* m_pollingRateLabel;

    QSlider* m_silenceThresholdSlider;
    QLabel* m_silenceThresholdLabel;

    QSlider* m_silenceTimeoutSlider;
    QLabel* m_silenceTimeoutLabel;

    void setupUi();
};

#endif // GENERAL_SETTINGS_VIEW_H
