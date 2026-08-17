#ifndef RESAMPLER_DETAIL_VIEW_H
#define RESAMPLER_DETAIL_VIEW_H

#include "models/AudioDeviceManager.h"
#include "models/AudioSettings.h"
#include "models/DSPEngineController.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QWidget>
#include <memory>

class ResamplerDetailView : public QWidget {
    Q_OBJECT

public:
    ResamplerDetailView(std::shared_ptr<AudioSettings> settings, std::shared_ptr<AudioDeviceManager> devices = nullptr,
                        std::shared_ptr<DSPEngineController> dspController = nullptr, QWidget* parent = nullptr);

private slots:
    void refreshUi();
    void applySettings();

private:
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<DSPEngineController> m_dspController;

    QWidget* m_contentWidget = nullptr;
    QFormLayout* m_settingsForm = nullptr;
    QGroupBox* m_settingsGroup = nullptr;

    QCheckBox* m_enabledCheck = nullptr;
    QComboBox* m_typeCombo = nullptr;
    QCheckBox* m_useProfileCheck = nullptr;
    QComboBox* m_profileCombo = nullptr;
    QSpinBox* m_threadsSpin = nullptr;
    QSpinBox* m_sincLenSpin = nullptr;
    QSpinBox* m_oversamplingSpin = nullptr;
    QComboBox* m_windowCombo = nullptr;
    QWidget* m_fCutoffRowWidget = nullptr;
    QSlider* m_fCutoffSlider = nullptr;
    QLabel* m_fCutoffLabel = nullptr;
    QComboBox* m_sincInterpCombo = nullptr;
    QComboBox* m_polyInterpCombo = nullptr;

    QLabel* m_capRateLabel = nullptr;
    QLabel* m_pbRateLabel = nullptr;
    QLabel* m_ratioLabel = nullptr;

    void updateVisibility();

    bool m_isLocalEditing = false;

    void setupUi();
};

#endif // RESAMPLER_DETAIL_VIEW_H
