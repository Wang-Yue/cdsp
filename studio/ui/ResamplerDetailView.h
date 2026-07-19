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

    QWidget* m_contentWidget;
    QFormLayout* m_typeForm;
    QGroupBox* m_typeGroup;

    QCheckBox* m_enabledCheck;
    QComboBox* m_typeCombo;
    QCheckBox* m_useProfileCheck;
    QComboBox* m_profileCombo;
    QSpinBox* m_sincLenSpin;
    QSpinBox* m_oversamplingSpin;
    QComboBox* m_windowCombo;
    QWidget* m_fCutoffRowWidget;
    QSlider* m_fCutoffSlider;
    QLabel* m_fCutoffLabel;
    QComboBox* m_sincInterpCombo;
    QComboBox* m_polyInterpCombo;

    QLabel* m_capRateLabel;
    QLabel* m_pbRateLabel;
    QLabel* m_ratioLabel;

    void updateVisibility();

    bool m_isLocalEditing = false;

    void setupUi();
};

#endif // RESAMPLER_DETAIL_VIEW_H
