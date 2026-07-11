#include "ui/ResamplerDetailView.h"

#include "ui/StyleTheme.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

ResamplerDetailView::ResamplerDetailView(std::shared_ptr<AudioSettings> settings,
                                         std::shared_ptr<AudioDeviceManager> devices,
                                         std::shared_ptr<DSPEngineController> dspController, QWidget* parent)
    : QWidget(parent), m_settings(settings), m_devices(devices), m_dspController(dspController) {
    setupUi();
    refreshUi();
}

void ResamplerDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(20);

    auto headerBox = new QHBoxLayout();
    headerBox->addWidget(new QLabel("Sample Rate Converter", this));
    headerBox->addStretch();

    m_enabledCheck = new QCheckBox("Enable Resampler", this);
    connect(m_enabledCheck, &QCheckBox::toggled, [this](bool checked) {
        m_settings->resamplerEnabled = checked;
        applySettings();
    });
    headerBox->addWidget(m_enabledCheck);
    mainLayout->addLayout(headerBox);

    auto container = this;
    m_typeGroup = new QGroupBox("Resampler Type & Parameters", container);
    m_typeForm = new QFormLayout(m_typeGroup);

    m_typeCombo = new QComboBox(m_typeGroup);
    m_typeCombo->addItems({"Synchronous", "AsyncSinc", "AsyncPoly", "Apple"});
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() {
        updateVisibility();
        applySettings();
    });
    m_typeForm->addRow("Type:", m_typeCombo);

    m_useProfileCheck = new QCheckBox("Use Quality Profile", m_typeGroup);
    connect(m_useProfileCheck, &QCheckBox::toggled, [this]() {
        updateVisibility();
        applySettings();
    });
    m_typeForm->addRow("", m_useProfileCheck);

    m_profileCombo = new QComboBox(m_typeGroup);
    m_profileCombo->addItems({"VeryFast", "Fast", "Balanced", "Accurate"});
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Quality Profile:", m_profileCombo);

    m_attenuationSpin = new QDoubleSpinBox(m_typeGroup);
    m_attenuationSpin->setRange(0.0, 60.0);
    m_attenuationSpin->setSingleStep(0.5);
    m_attenuationSpin->setSuffix(" dB");
    connect(m_attenuationSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Attenuation:", m_attenuationSpin);

    m_sincLenSpin = new QSpinBox(m_typeGroup);
    m_sincLenSpin->setRange(16, 4096);
    connect(m_sincLenSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Sinc Length:", m_sincLenSpin);

    m_oversamplingSpin = new QSpinBox(m_typeGroup);
    m_oversamplingSpin->setRange(16, 2048);
    connect(m_oversamplingSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Oversampling Factor:", m_oversamplingSpin);

    m_windowCombo = new QComboBox(m_typeGroup);
    m_windowCombo->addItems({"Blackman", "Blackman2", "BlackmanHarris", "BlackmanHarris2", "Hann", "Hann2"});
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Window Function:", m_windowCombo);

    m_fCutoffSpin = new QDoubleSpinBox(m_typeGroup);
    m_fCutoffSpin->setRange(0.5, 0.99);
    m_fCutoffSpin->setSingleStep(0.01);
    m_fCutoffSpin->setSuffix(" × Fs/2");
    connect(m_fCutoffSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Cutoff Frequency Ratio:", m_fCutoffSpin);

    m_sincInterpCombo = new QComboBox(m_typeGroup);
    m_sincInterpCombo->addItems({"Linear", "Quadratic", "Cubic"});
    connect(m_sincInterpCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Sinc Interpolation:", m_sincInterpCombo);

    m_polyInterpCombo = new QComboBox(m_typeGroup);
    m_polyInterpCombo->addItems({"Linear", "Quadratic", "Cubic"});
    connect(m_polyInterpCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Poly Interpolation:", m_polyInterpCombo);

    m_appleQualityCombo = new QComboBox(m_typeGroup);
    m_appleQualityCombo->addItems({"Min", "Low", "Medium", "High", "Max"});
    connect(m_appleQualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Apple Quality:", m_appleQualityCombo);

    m_appleComplexityCombo = new QComboBox(m_typeGroup);
    m_appleComplexityCombo->addItems({"Normal", "Mastering"});
    connect(m_appleComplexityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Apple Algorithm:", m_appleComplexityCombo);

    mainLayout->addWidget(m_typeGroup);

    // Sample Rates Info Card
    auto ratesGroup = new QGroupBox("Sample Rates", container);
    auto ratesForm = new QFormLayout(ratesGroup);

    m_capRateLabel = new QLabel("44.1 kHz", ratesGroup);
    ratesForm->addRow("Capture Sample Rate:", m_capRateLabel);

    m_pbRateLabel = new QLabel("48.0 kHz", ratesGroup);
    ratesForm->addRow("Playback Sample Rate:", m_pbRateLabel);

    m_ratioLabel = new QLabel("Conversion Ratio: 1.0884", ratesGroup);
    m_ratioLabel->setStyleSheet("color: #8e8e93; font-size: 11px;");
    ratesForm->addRow("", m_ratioLabel);

    mainLayout->addWidget(ratesGroup);

    auto footnoteLbl = new QLabel(
        "Resamples audio between capture and playback sample rates. Configure sample rates in the Devices page.",
        container);
    footnoteLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
    footnoteLbl->setWordWrap(true);
    mainLayout->addWidget(footnoteLbl);

    mainLayout->addStretch();
}

void ResamplerDetailView::updateVisibility() {
    if (!m_typeForm)
        return;

    std::string typeStr = m_typeCombo->currentText().toStdString();
    bool isAsyncSinc = (typeStr == "AsyncSinc");
    bool isAsyncPoly = (typeStr == "AsyncPoly");
    bool isApple = (typeStr == "Apple");
    bool useProfile = m_useProfileCheck->isChecked();

    m_typeForm->setRowVisible(m_useProfileCheck, isAsyncSinc);
    m_typeForm->setRowVisible(m_profileCombo, isAsyncSinc && useProfile);
    m_typeForm->setRowVisible(m_attenuationSpin, isAsyncSinc || isAsyncPoly);

    m_typeForm->setRowVisible(m_sincLenSpin, isAsyncSinc && !useProfile);
    m_typeForm->setRowVisible(m_oversamplingSpin, isAsyncSinc && !useProfile);
    m_typeForm->setRowVisible(m_windowCombo, isAsyncSinc && !useProfile);
    m_typeForm->setRowVisible(m_fCutoffSpin, isAsyncSinc && !useProfile);
    m_typeForm->setRowVisible(m_sincInterpCombo, isAsyncSinc && !useProfile);

    m_typeForm->setRowVisible(m_polyInterpCombo, isAsyncPoly);

    m_typeForm->setRowVisible(m_appleQualityCombo, isApple);
    m_typeForm->setRowVisible(m_appleComplexityCombo, isApple);
}

void ResamplerDetailView::refreshUi() {
    m_enabledCheck->setChecked(m_settings->resamplerEnabled);
    if (m_typeGroup)
        m_typeGroup->setEnabled(m_settings->resamplerEnabled);
    m_typeCombo->setCurrentText(QString::fromStdString(resamplerTypeToString(m_settings->resamplerType)));
    m_useProfileCheck->setChecked(m_settings->resamplerUseProfile);
    m_profileCombo->setCurrentText(QString::fromStdString(resamplerProfileToString(m_settings->resamplerProfile)));
    m_attenuationSpin->setValue(m_settings->resamplerAttenuation);
    m_sincLenSpin->setValue(m_settings->resamplerSincLen);
    m_oversamplingSpin->setValue(m_settings->resamplerOversamplingFactor);
    m_windowCombo->setCurrentText(QString::fromStdString(m_settings->resamplerWindow));
    m_fCutoffSpin->setValue(m_settings->resamplerFCutoff);

    if (m_devices) {
        int capRate = m_devices->captureConfig.sampleRate > 0 ? m_devices->captureConfig.sampleRate : 44100;
        int pbRate = m_devices->playbackConfig.sampleRate > 0 ? m_devices->playbackConfig.sampleRate : 48000;
        m_capRateLabel->setText(capRate >= 1000 ? QString("%1 kHz").arg(capRate / 1000.0, 0, 'f', 1)
                                                : QString("%1 Hz").arg(capRate));
        m_pbRateLabel->setText(pbRate >= 1000 ? QString("%1 kHz").arg(pbRate / 1000.0, 0, 'f', 1)
                                              : QString("%1 Hz").arg(pbRate));
        double ratio = static_cast<double>(pbRate) / static_cast<double>(capRate);
        m_ratioLabel->setText(QString("Conversion Ratio: %1").arg(ratio, 0, 'f', 4));
    }

    updateVisibility();
}

void ResamplerDetailView::applySettings() {
    m_settings->resamplerEnabled = m_enabledCheck->isChecked();
    m_settings->resamplerType = stringToResamplerType(m_typeCombo->currentText().toStdString());
    m_settings->resamplerUseProfile = m_useProfileCheck->isChecked();
    m_settings->resamplerProfile = stringToResamplerProfile(m_profileCombo->currentText().toStdString());
    m_settings->resamplerAttenuation = m_attenuationSpin->value();
    m_settings->resamplerSincLen = m_sincLenSpin->value();
    m_settings->resamplerOversamplingFactor = m_oversamplingSpin->value();
    m_settings->resamplerWindow = m_windowCombo->currentText().toStdString();
    m_settings->resamplerFCutoff = m_fCutoffSpin->value();
    m_settings->savePreferences();

    if (m_dspController) {
        m_dspController->applyConfig();
    }
}
