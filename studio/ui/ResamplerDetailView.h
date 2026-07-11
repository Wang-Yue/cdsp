#ifndef RESAMPLER_DETAIL_VIEW_H
#define RESAMPLER_DETAIL_VIEW_H

#include "models/AudioSettings.h"
#include "models/AudioDeviceManager.h"
#include <QWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QFormLayout>
#include <memory>

class ResamplerDetailView : public QWidget {
    Q_OBJECT

public:
    ResamplerDetailView(
        std::shared_ptr<AudioSettings> settings,
        std::shared_ptr<AudioDeviceManager> devices = nullptr,
        QWidget* parent = nullptr
    );

private slots:
    void refreshUi();
    void applySettings();

private:
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<AudioDeviceManager> m_devices;

    QFormLayout* m_typeForm;

    QCheckBox* m_enabledCheck;
    QComboBox* m_typeCombo;
    QCheckBox* m_useProfileCheck;
    QComboBox* m_profileCombo;
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
