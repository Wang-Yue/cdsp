#include "ui/ResamplerDetailView.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>

ResamplerDetailView::ResamplerDetailView(std::shared_ptr<AudioSettings> settings, QWidget* parent)
    : QWidget(parent), m_settings(settings) {
    setupUi();
    refreshUi();
}

void ResamplerDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(20);

    auto group = new QGroupBox("Resampler Engine Settings", this);
    auto form = new QFormLayout(group);

    m_enabledCheck = new QCheckBox("Enable Resampler", group);
    form->addRow("", m_enabledCheck);

    m_typeCombo = new QComboBox(group);
    m_typeCombo->addItems({"Synchronous", "Apple", "AsyncSinc", "AsyncPoly"});
    form->addRow("Resampler Type:", m_typeCombo);

    m_profileCombo = new QComboBox(group);
    m_profileCombo->addItems({"VeryFast", "Fast", "Balanced", "Accurate"});
    form->addRow("Quality Profile:", m_profileCombo);

    m_sincLenSpin = new QSpinBox(group);
    m_sincLenSpin->setRange(16, 4096);
    form->addRow("Sinc Length:", m_sincLenSpin);

    m_oversamplingSpin = new QSpinBox(group);
    m_oversamplingSpin->setRange(16, 2048);
    form->addRow("Oversampling Factor:", m_oversamplingSpin);

    m_fCutoffSpin = new QDoubleSpinBox(group);
    m_fCutoffSpin->setRange(0.5, 0.99);
    m_fCutoffSpin->setSingleStep(0.01);
    form->addRow("Cutoff Frequency Ratio:", m_fCutoffSpin);

    mainLayout->addWidget(group);

    auto applyBtn = new QPushButton("Apply Resampler Settings", this);
    connect(applyBtn, &QPushButton::clicked, this, &ResamplerDetailView::applySettings);
    mainLayout->addWidget(applyBtn);

    mainLayout->addStretch();
}

void ResamplerDetailView::refreshUi() {
    m_enabledCheck->setChecked(m_settings->resamplerEnabled);
    m_typeCombo->setCurrentText(QString::fromStdString(resamplerTypeToString(m_settings->resamplerType)));
    m_profileCombo->setCurrentText(QString::fromStdString(resamplerProfileToString(m_settings->resamplerProfile)));
    m_sincLenSpin->setValue(m_settings->resamplerSincLen);
    m_oversamplingSpin->setValue(m_settings->resamplerOversamplingFactor);
    m_fCutoffSpin->setValue(m_settings->resamplerFCutoff);
}

void ResamplerDetailView::applySettings() {
    m_settings->resamplerEnabled = m_enabledCheck->isChecked();
    m_settings->resamplerType = stringToResamplerType(m_typeCombo->currentText().toStdString());
    m_settings->resamplerProfile = stringToResamplerProfile(m_profileCombo->currentText().toStdString());
    m_settings->resamplerSincLen = m_sincLenSpin->value();
    m_settings->resamplerOversamplingFactor = m_oversamplingSpin->value();
    m_settings->resamplerFCutoff = m_fCutoffSpin->value();
    m_settings->savePreferences();
}
