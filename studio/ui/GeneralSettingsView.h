#ifndef GENERAL_SETTINGS_VIEW_H
#define GENERAL_SETTINGS_VIEW_H

#include "models/AudioSettings.h"        // for AudioSettings
#include "models/MonitoringController.h" // for MonitoringController

#include <QLabel>  // for QLabel
#include <QObject> // for Q_OBJECT, slots
#include <QSlider> // for QSlider
#include <QWidget> // for QWidget
#include <memory>  // for shared_ptr

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

    QSlider* m_pollingRateSlider = nullptr;
    QLabel* m_pollingRateLabel = nullptr;

    QSlider* m_silenceThresholdSlider = nullptr;
    QLabel* m_silenceThresholdLabel = nullptr;

    QSlider* m_silenceTimeoutSlider = nullptr;
    QLabel* m_silenceTimeoutLabel = nullptr;

    void setupUi();
};

#endif // GENERAL_SETTINGS_VIEW_H
