#include "ui/ResamplerDetailView.h"

#include "config/DSPConfigTypes.h"
#include "ui/StyleTheme.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>

ResamplerDetailView::ResamplerDetailView(std::shared_ptr<AudioSettings> settings,
                                         std::shared_ptr<AudioDeviceManager> devices,
                                         std::shared_ptr<DSPEngineController> dspController, QWidget* parent)
    : QWidget(parent), m_settings(settings), m_devices(devices), m_dspController(dspController) {
    setupUi();
    if (m_devices) {
        connect(m_devices.get(), &AudioDeviceManager::configChanged, this, &ResamplerDetailView::refreshUi);
    }
    refreshUi();
}

void ResamplerDetailView::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(20);

    auto headerBox = new QHBoxLayout();
    headerBox->addWidget(new QLabel("Sample Rate Converter", this));
    headerBox->addStretch();

    m_enabledCheck = new QCheckBox("Enabled", this);
    connect(m_enabledCheck, &QCheckBox::toggled, [this](bool checked) {
        m_settings->resamplerEnabled = checked;
        applySettings();
    });
    if (m_settings) {
        connect(m_settings.get(), &AudioSettings::settingsChanged, this, &ResamplerDetailView::refreshUi);
    }
    headerBox->addWidget(m_enabledCheck);
    mainLayout->addLayout(headerBox);

    m_contentWidget = new QWidget(this);
    auto contentLayout = new QVBoxLayout(m_contentWidget);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(20);

    m_typeGroup = new QGroupBox("Resampler Type", m_contentWidget);
    m_typeForm = new QFormLayout(m_typeGroup);
    m_typeForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_typeCombo = new QComboBox(m_typeGroup);
    m_typeCombo->addItems({"AsyncSinc", "AsyncPoly", "Synchronous"});
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() {
        updateVisibility();
        applySettings();
    });
    m_typeForm->addRow("Type:", m_typeCombo);

    m_useProfileCheck = new QCheckBox("Use Profile", m_typeGroup);
    connect(m_useProfileCheck, &QCheckBox::toggled, [this]() {
        updateVisibility();
        applySettings();
    });
    m_typeForm->addRow("", m_useProfileCheck);

    m_profileCombo = new QComboBox(m_typeGroup);
    m_profileCombo->addItems({"VeryFast", "Fast", "Balanced", "Accurate"});
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Profile:", m_profileCombo);

    m_sincLenSpin = new QSpinBox(m_typeGroup);
    m_sincLenSpin->setRange(16, 4096);
    connect(m_sincLenSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Sinc Length:", m_sincLenSpin);

    m_oversamplingSpin = new QSpinBox(m_typeGroup);
    m_oversamplingSpin->setRange(16, 2048);
    connect(m_oversamplingSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Oversampling:", m_oversamplingSpin);

    m_windowCombo = new QComboBox(m_typeGroup);
    m_windowCombo->addItem("Blackman", "Blackman");
    m_windowCombo->addItem("Blackman 2", "Blackman2");
    m_windowCombo->addItem("Blackman-Harris", "BlackmanHarris");
    m_windowCombo->addItem("Blackman-Harris 2", "BlackmanHarris2");
    m_windowCombo->addItem("Hann", "Hann");
    m_windowCombo->addItem("Hann 2", "Hann2");
    connect(m_windowCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Window:", m_windowCombo);

    m_fCutoffRowWidget = new QWidget(m_typeGroup);
    auto cutoffLayout = new QHBoxLayout(m_fCutoffRowWidget);
    cutoffLayout->setContentsMargins(0, 0, 0, 0);
    cutoffLayout->setSpacing(12);

    m_fCutoffSlider = new QSlider(Qt::Horizontal, m_fCutoffRowWidget);
    m_fCutoffSlider->setRange(50, 99);
    m_fCutoffSlider->setFixedWidth(200);

    m_fCutoffLabel = new QLabel("0.95 × Fs/2", m_fCutoffRowWidget);
    m_fCutoffLabel->setFont(QFont("monospace", 11));

    connect(m_fCutoffSlider, &QSlider::valueChanged, [this](int val) {
        double fRatio = val / 100.0;
        m_fCutoffLabel->setText(QString("%1 × Fs/2").arg(fRatio, 0, 'f', 2));
        applySettings();
    });

    cutoffLayout->addWidget(m_fCutoffSlider);
    cutoffLayout->addWidget(m_fCutoffLabel);
    cutoffLayout->addStretch();

    m_typeForm->addRow("Cutoff Freq:", m_fCutoffRowWidget);

    m_sincInterpCombo = new QComboBox(m_typeGroup);
    m_sincInterpCombo->addItems({"Nearest", "Linear", "Quadratic", "Cubic"});
    connect(m_sincInterpCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Interpolation:", m_sincInterpCombo);

    m_polyInterpCombo = new QComboBox(m_typeGroup);
    m_polyInterpCombo->addItems({"Linear", "Cubic", "Quintic", "Septic"});
    connect(m_polyInterpCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { applySettings(); });
    m_typeForm->addRow("Interp:", m_polyInterpCombo);

    contentLayout->addWidget(m_typeGroup);

    // Sample Rates Info Card
    auto ratesGroup = new QGroupBox("Sample Rates", m_contentWidget);
    auto ratesForm = new QFormLayout(ratesGroup);

    m_capRateLabel = new QLabel("44.1 kHz", ratesGroup);
    ratesForm->addRow("Capture:", m_capRateLabel);

    m_pbRateLabel = new QLabel("48.0 kHz", ratesGroup);
    ratesForm->addRow("Playback:", m_pbRateLabel);

    m_ratioLabel = new QLabel("Conversion ratio: 1.0884", ratesGroup);
    m_ratioLabel->setStyleSheet("color: #8e8e93; font-size: 11px;");
    ratesForm->addRow("", m_ratioLabel);

    contentLayout->addWidget(ratesGroup);

    auto footnoteLbl = new QLabel(
        "Resamples audio between capture and playback sample rates. Configure sample rates in the Devices page.",
        m_contentWidget);
    footnoteLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
    footnoteLbl->setWordWrap(true);
    contentLayout->addWidget(footnoteLbl);

    mainLayout->addWidget(m_contentWidget);
    mainLayout->addStretch();
}

void ResamplerDetailView::updateVisibility() {
    if (!m_typeForm)
        return;

    std::string typeStr = m_typeCombo->currentText().toStdString();
    bool isAsyncSinc = (typeStr == "AsyncSinc");
    bool isAsyncPoly = (typeStr == "AsyncPoly");
    bool useProfile = m_useProfileCheck->isChecked();

    m_typeForm->setRowVisible(m_useProfileCheck, isAsyncSinc);
    m_typeForm->setRowVisible(m_profileCombo, isAsyncSinc && useProfile);

    m_typeForm->setRowVisible(m_sincLenSpin, isAsyncSinc && !useProfile);
    m_typeForm->setRowVisible(m_oversamplingSpin, isAsyncSinc && !useProfile);
    m_typeForm->setRowVisible(m_windowCombo, isAsyncSinc && !useProfile);
    m_typeForm->setRowVisible(m_fCutoffRowWidget, isAsyncSinc && !useProfile);
    m_typeForm->setRowVisible(m_sincInterpCombo, isAsyncSinc && !useProfile);

    m_typeForm->setRowVisible(m_polyInterpCombo, isAsyncPoly);

    if (m_contentWidget && m_settings) {
        bool enabled = m_settings->resamplerEnabled;
        m_contentWidget->setEnabled(enabled);
    }
}

void ResamplerDetailView::refreshUi() {
    if (m_isLocalEditing)
        return;
    m_enabledCheck->setChecked(m_settings->resamplerEnabled);

    bool allowSlip = false;
    if (m_devices) {
        int capRate = m_devices->captureConfig.sampleRate > 0 ? m_devices->captureConfig.sampleRate : 44100;
        int pbRate = m_devices->playbackConfig.sampleRate > 0 ? m_devices->playbackConfig.sampleRate : 48000;
        if (capRate == pbRate) {
            allowSlip = true;
        }
    }

    if (!allowSlip && m_settings->resamplerType == ResamplerType::Slip) {
        m_settings->resamplerType = ResamplerType::Synchronous;
    }

    m_typeCombo->blockSignals(true);
    m_typeCombo->clear();
    m_typeCombo->addItems({"AsyncSinc", "AsyncPoly", "Synchronous"});
    if (allowSlip) {
        m_typeCombo->addItem("Slip");
    }
    m_typeCombo->blockSignals(false);

    m_typeCombo->setCurrentText(QString::fromStdString(resamplerTypeToString(m_settings->resamplerType)));
    updateVisibility();
    m_useProfileCheck->setChecked(m_settings->resamplerUseProfile);
    m_profileCombo->setCurrentText(QString::fromStdString(resamplerProfileToString(m_settings->resamplerProfile)));
    m_sincLenSpin->setValue(m_settings->resamplerSincLen);
    m_oversamplingSpin->setValue(m_settings->resamplerOversamplingFactor);

    int winIdx = m_windowCombo->findData(QString::fromStdString(m_settings->resamplerWindow));
    if (winIdx >= 0) {
        m_windowCombo->setCurrentIndex(winIdx);
    } else {
        m_windowCombo->setCurrentIndex(0);
    }

    int cutoffVal = static_cast<int>(std::round(m_settings->resamplerFCutoff * 100.0));
    m_fCutoffSlider->blockSignals(true);
    m_fCutoffSlider->setValue(cutoffVal);
    m_fCutoffSlider->blockSignals(false);
    m_fCutoffLabel->setText(QString("%1 × Fs/2").arg(m_settings->resamplerFCutoff, 0, 'f', 2));

    m_sincInterpCombo->setCurrentText(
        QString::fromStdString(sincInterpolationToString(m_settings->resamplerSincInterpolation)));
    m_polyInterpCombo->setCurrentText(
        QString::fromStdString(resamplerInterpolationToString(m_settings->resamplerInterpolation)));

    if (m_devices) {
        int capRate = m_devices->captureConfig.sampleRate > 0 ? m_devices->captureConfig.sampleRate : 44100;
        int pbRate = m_devices->playbackConfig.sampleRate > 0 ? m_devices->playbackConfig.sampleRate : 48000;
        m_capRateLabel->setText(capRate >= 1000 ? QString("%1 kHz").arg(capRate / 1000.0, 0, 'f', 1)
                                                : QString("%1 Hz").arg(capRate));
        m_pbRateLabel->setText(pbRate >= 1000 ? QString("%1 kHz").arg(pbRate / 1000.0, 0, 'f', 1)
                                              : QString("%1 Hz").arg(pbRate));
        double ratio = static_cast<double>(pbRate) / static_cast<double>(capRate);
        m_ratioLabel->setText(QString("Conversion ratio: %1").arg(ratio, 0, 'f', 4));
    }

    updateVisibility();
}

void ResamplerDetailView::applySettings() {
    m_isLocalEditing = true;
    m_settings->resamplerEnabled = m_enabledCheck->isChecked();
    m_settings->resamplerType = stringToResamplerType(m_typeCombo->currentText().toStdString());
    m_settings->resamplerUseProfile = m_useProfileCheck->isChecked();
    m_settings->resamplerProfile = stringToResamplerProfile(m_profileCombo->currentText().toStdString());
    m_settings->resamplerSincLen = m_sincLenSpin->value();
    m_settings->resamplerOversamplingFactor = m_oversamplingSpin->value();
    m_settings->resamplerWindow = m_windowCombo->currentData().toString().toStdString();
    m_settings->resamplerFCutoff = m_fCutoffSlider->value() / 100.0;
    m_settings->resamplerSincInterpolation = stringToSincInterpolation(m_sincInterpCombo->currentText().toStdString());
    m_settings->resamplerInterpolation = stringToResamplerInterpolation(m_polyInterpCombo->currentText().toStdString());
    m_settings->savePreferences();

    if (m_dspController) {
        m_dspController->applyConfig();
    }
    m_isLocalEditing = false;
}
