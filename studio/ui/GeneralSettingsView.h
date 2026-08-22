#ifndef GENERAL_SETTINGS_VIEW_H
#define GENERAL_SETTINGS_VIEW_H

#include "models/AudioSettings.h"
#include "models/MonitoringController.h"

#include <QLabel>
#include <QShowEvent>
#include <QSlider>
#include <QWidget>
#include <memory>

class QCheckBox;
class QComboBox;

class GeneralSettingsView : public QWidget {
    Q_OBJECT

public:
    GeneralSettingsView(std::shared_ptr<AudioSettings> settings, std::shared_ptr<MonitoringController> monitoring,
                        QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void refreshUi();

private:
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<MonitoringController> m_monitoring;

    QComboBox* m_themeCombo = nullptr;

    QSlider* m_pollingRateSlider = nullptr;
    QLabel* m_pollingRateLabel = nullptr;

    QSlider* m_silenceThresholdSlider = nullptr;
    QLabel* m_silenceThresholdLabel = nullptr;

    QSlider* m_silenceTimeoutSlider = nullptr;
    QLabel* m_silenceTimeoutLabel = nullptr;

    QCheckBox* m_closeToTrayCheck = nullptr;
    QCheckBox* m_minimizeToTrayCheck = nullptr;

    void setupUi();
};

#endif // GENERAL_SETTINGS_VIEW_H
