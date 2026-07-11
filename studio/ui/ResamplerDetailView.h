#ifndef RESAMPLER_DETAIL_VIEW_H
#define RESAMPLER_DETAIL_VIEW_H

#include "models/AudioSettings.h"
#include <QWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <memory>

class ResamplerDetailView : public QWidget {
    Q_OBJECT

public:
    ResamplerDetailView(std::shared_ptr<AudioSettings> settings, QWidget* parent = nullptr);

private slots:
    void refreshUi();
    void applySettings();

private:
    std::shared_ptr<AudioSettings> m_settings;

    QCheckBox* m_enabledCheck;
    QComboBox* m_typeCombo;
    QComboBox* m_profileCombo;
    QSpinBox* m_sincLenSpin;
    QSpinBox* m_oversamplingSpin;
    QDoubleSpinBox* m_fCutoffSpin;

    void setupUi();
};

#endif // RESAMPLER_DETAIL_VIEW_H
