#ifndef RESAMPLER_DETAIL_VIEW_H
#define RESAMPLER_DETAIL_VIEW_H

#include "models/AudioSettings.h"
#include "models/AudioDeviceManager.h"
#include "models/DSPEngineController.h"
#include <QWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QFormLayout>
#include <memory>

class ResamplerDetailView : public QWidget {
    Q_OBJECT

public:
    ResamplerDetailView(
        std::shared_ptr<AudioSettings> settings,
        std::shared_ptr<AudioDeviceManager> devices = nullptr,
        std::shared_ptr<DSPEngineController> dspController = nullptr,
        QWidget* parent = nullptr
    );

private slots:
    void refreshUi();
    void applySettings();

private:
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<AudioDeviceManager> m_devices;
    std::shared_ptr<DSPEngineController> m_dspController;

    QFormLayout* m_typeForm;
    QGroupBox* m_typeGroup;

    QCheckBox* m_enabledCheck;
    QComboBox* m_typeCombo;
    QCheckBox* m_useProfileCheck;
    QComboBox* m_profileCombo;
    QDoubleSpinBox* m_attenuationSpin;
    QSpinBox* m_sincLenSpin;
    QSpinBox* m_oversamplingSpin;
    QComboBox* m_windowCombo;
    QDoubleSpinBox* m_fCutoffSpin;
    QComboBox* m_sincInterpCombo;
    QComboBox* m_polyInterpCombo;
    QComboBox* m_appleQualityCombo;
    QComboBox* m_appleComplexityCombo;

    QLabel* m_capRateLabel;
    QLabel* m_pbRateLabel;
    QLabel* m_ratioLabel;

    void updateVisibility();

    void setupUi();
};

#endif // RESAMPLER_DETAIL_VIEW_H
