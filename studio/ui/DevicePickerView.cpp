#include "ui/DevicePickerView.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QFileDialog>

DevicePickerView::DevicePickerView(
    std::shared_ptr<AudioDeviceManager> devices,
    std::shared_ptr<AudioSettings> settings,
    QWidget* parent
) : QWidget(parent), m_devices(devices), m_settings(settings) {
    setupUi();

    connect(m_devices.get(), &AudioDeviceManager::devicesRefreshed, this, &DevicePickerView::refreshUi);
    connect(m_devices.get(), &AudioDeviceManager::configChanged, this, &DevicePickerView::refreshUi);

    refreshUi();
}

void DevicePickerView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(20);

    auto title = new QLabel("Audio Device Settings", container);
    title->setFont(QFont("sans-serif", 16, QFont::Bold));
    mainLayout->addWidget(title);

    // 1. Capture Group
    auto capGroup = new QGroupBox("Capture (Input)", container);
    auto capLayout = new QVBoxLayout(capGroup);

    auto capBackendBox = new QHBoxLayout();
    capBackendBox->addWidget(new QLabel("Backend:", capGroup));
    m_capBackendCombo = new QComboBox(capGroup);
    m_capBackendCombo->addItems({"CoreAudio", "RawFile", "WavFile", "SignalGenerator"});
    connect(m_capBackendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_capStack->setCurrentIndex(idx);
    });
    capBackendBox->addWidget(m_capBackendCombo);
    capBackendBox->addStretch();
    capLayout->addLayout(capBackendBox);

    m_capStack = new QStackedWidget(capGroup);
    m_capStack->addWidget(createCapCoreAudioView());
    m_capStack->addWidget(createCapFileView(false));
    m_capStack->addWidget(createCapFileView(true));
    m_capStack->addWidget(createCapGeneratorView());
    capLayout->addWidget(m_capStack);

    mainLayout->addWidget(capGroup);

    // 2. Playback Group
    auto pbGroup = new QGroupBox("Playback (Output)", container);
    auto pbLayout = new QVBoxLayout(pbGroup);

    auto pbBackendBox = new QHBoxLayout();
    pbBackendBox->addWidget(new QLabel("Backend:", pbGroup));
    m_pbBackendCombo = new QComboBox(pbGroup);
    m_pbBackendCombo->addItems({"CoreAudio", "RawFile"});
    connect(m_pbBackendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_pbStack->setCurrentIndex(idx);
    });
    pbBackendBox->addWidget(m_pbBackendCombo);
    pbBackendBox->addStretch();
    pbLayout->addLayout(pbBackendBox);

    m_pbStack = new QStackedWidget(pbGroup);
    m_pbStack->addWidget(createPbCoreAudioView());
    m_pbStack->addWidget(createPbFileView());
    pbLayout->addWidget(m_pbStack);

    mainLayout->addWidget(pbGroup);

    // 3. Processing Group
    auto procGroup = new QGroupBox("Processing", container);
    auto procForm = new QFormLayout(procGroup);

    auto chunkLayout = new QHBoxLayout();
    m_chunkSizeCombo = new QComboBox(procGroup);
    for (int size : {256, 512, 1024, 2048, 4096, 8192, 16384, 32768}) {
        m_chunkSizeCombo->addItem(QString("%1 samples").arg(size), size);
    }
    chunkLayout->addWidget(m_chunkSizeCombo);

    auto latencyLbl = new QLabel("(10.7 ms latency)", procGroup);
    latencyLbl->setStyleSheet("color: #8e8e93; font-style: italic;");
    auto updateLatencyText = [this, latencyLbl]() {
        int chunkSize = m_chunkSizeCombo->currentData().toInt();
        double sampleRate = m_devices->captureConfig.sampleRate > 0 ? m_devices->captureConfig.sampleRate : 48000.0;
        double ms = (chunkSize * 1000.0) / sampleRate;
        latencyLbl->setText(QString("(%1 ms latency)").arg(ms, 0, 'f', 1));
    };
    updateLatencyText();
    connect(m_chunkSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), updateLatencyText);
    chunkLayout->addWidget(latencyLbl);
    chunkLayout->addStretch();
    procForm->addRow("Chunk Size:", chunkLayout);

    m_enableRateAdjustCheck = new QCheckBox("Enable Dynamic Rate Adjustment", procGroup);
    procForm->addRow("", m_enableRateAdjustCheck);
    auto rateAdjustNote = new QLabel("Compensate for clock drift between capture and playback devices", procGroup);
    rateAdjustNote->setStyleSheet("color: #8e8e93; font-size: 11px;");
    procForm->addRow("", rateAdjustNote);

    m_queueLimitSpin = new QSpinBox(procGroup);
    m_queueLimitSpin->setRange(1, 32);
    procForm->addRow("Queue Limit:", m_queueLimitSpin);

    m_stopOnRateChangeCheck = new QCheckBox("Stop on Rate Change", procGroup);
    procForm->addRow("", m_stopOnRateChangeCheck);

    m_measureIntervalSpin = new QDoubleSpinBox(procGroup);
    m_measureIntervalSpin->setRange(0.1, 10.0);
    m_measureIntervalSpin->setSingleStep(0.1);
    m_measureIntervalSpin->setSuffix(" s");
    procForm->addRow("Rate Measure Interval:", m_measureIntervalSpin);

    m_multithreadedCheck = new QCheckBox("Multithreaded Processing Engine", procGroup);
    procForm->addRow("", m_multithreadedCheck);

    m_workerThreadsSpin = new QSpinBox(procGroup);
    m_workerThreadsSpin->setRange(0, 32);
    procForm->addRow("Worker Threads (0 = Auto):", m_workerThreadsSpin);

    mainLayout->addWidget(procGroup);

    auto btnBox = new QHBoxLayout();
    auto refreshBtn = new QPushButton("Refresh Devices", container);
    connect(refreshBtn, &QPushButton::clicked, [this]() {
        m_devices->fetchDevices();
    });
    btnBox->addWidget(refreshBtn);

    btnBox->addStretch();

    auto applyBtn = new QPushButton("Apply Hardware Settings", container);
    applyBtn->setStyleSheet("background-color: #007af5; color: white;");
    connect(applyBtn, &QPushButton::clicked, this, &DevicePickerView::applySettings);
    btnBox->addWidget(applyBtn);

    mainLayout->addLayout(btnBox);
    scroll->setWidget(container);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

QWidget* DevicePickerView::createCapCoreAudioView() {
    auto w = new QWidget(this);
    auto form = new QFormLayout(w);

    m_capDeviceCombo = new QComboBox(w);
    connect(m_capDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        Q_UNUSED(idx);
        applySettings();
    });
    form->addRow("Device:", m_capDeviceCombo);

    auto chBox = new QHBoxLayout();
    m_capDevChannelsSpin = new QSpinBox(w);
    m_capDevChannelsSpin->setRange(1, 32);
    chBox->addWidget(new QLabel("Device Ch:"));
    chBox->addWidget(m_capDevChannelsSpin);

    m_capStreamChannelsSpin = new QSpinBox(w);
    m_capStreamChannelsSpin->setRange(1, 32);
    chBox->addWidget(new QLabel("Stream Ch:"));
    chBox->addWidget(m_capStreamChannelsSpin);
    form->addRow("Channels:", chBox);

    m_capRateCombo = new QComboBox(w);
    form->addRow("Sample Rate:", m_capRateCombo);

    m_capFormatCombo = new QComboBox(w);
    form->addRow("Sample Format:", m_capFormatCombo);

    m_bypassDoPCheck = new QCheckBox("Bypass DoP Detection", w);
    form->addRow("", m_bypassDoPCheck);

    m_dopCutoffCombo = new QComboBox(w);
    for (int f : {20000, 25000, 30000, 40000, 50000}) {
        m_dopCutoffCombo->addItem(QString("%1 kHz").arg(f / 1000), f);
    }
    form->addRow("DoP Cutoff Frequency:", m_dopCutoffCombo);

    return w;
}

QWidget* DevicePickerView::createCapFileView(bool isWav) {
    auto w = new QWidget(this);
    auto form = new QFormLayout(w);

    auto fileBox = new QHBoxLayout();
    m_capFilePathEdit = new QLineEdit(w);
    fileBox->addWidget(m_capFilePathEdit);

    auto browseBtn = new QPushButton("Browse...", w);
    connect(browseBtn, &QPushButton::clicked, [this, isWav, w]() {
        QString path = QFileDialog::getOpenFileName(w, "Select File", "", isWav ? "WAV Files (*.wav)" : "Raw Files (*.raw *.f64 *.f32)");
        if (!path.isEmpty()) m_capFilePathEdit->setText(path);
    });
    fileBox->addWidget(browseBtn);
    form->addRow("File Path:", fileBox);

    m_capFileFormatCombo = new QComboBox(w);
    m_capFileFormatCombo->addItems({"S16_LE", "S24_3_LE", "S24_4_RJ_LE", "S24_4_LJ_LE", "S32_LE", "F32_LE", "F64_LE"});
    form->addRow("Format:", m_capFileFormatCombo);

    m_capFileChannelsSpin = new QSpinBox(w);
    m_capFileChannelsSpin->setRange(1, 32);
    form->addRow("Channels:", m_capFileChannelsSpin);

    m_capSkipBytesSpin = new QSpinBox(w);
    m_capSkipBytesSpin->setRange(0, 1000000);
    form->addRow("Skip Bytes:", m_capSkipBytesSpin);

    m_capReadBytesSpin = new QSpinBox(w);
    m_capReadBytesSpin->setRange(0, 100000000);
    form->addRow("Read Bytes (0 = All):", m_capReadBytesSpin);

    m_capExtraSamplesSpin = new QSpinBox(w);
    m_capExtraSamplesSpin->setRange(0, 1000000);
    form->addRow("Extra Samples:", m_capExtraSamplesSpin);

    return w;
}

QWidget* DevicePickerView::createCapGeneratorView() {
    auto w = new QWidget(this);
    auto form = new QFormLayout(w);

    m_genTypeCombo = new QComboBox(w);
    m_genTypeCombo->addItems({"Sine", "Square", "WhiteNoise"});
    form->addRow("Signal Type:", m_genTypeCombo);

    m_genChannelsSpin = new QSpinBox(w);
    m_genChannelsSpin->setRange(1, 32);
    form->addRow("Channels:", m_genChannelsSpin);

    m_genFreqSpin = new QDoubleSpinBox(w);
    m_genFreqSpin->setRange(1.0, 20000.0);
    m_genFreqSpin->setSuffix(" Hz");
    form->addRow("Frequency:", m_genFreqSpin);

    m_genLevelSpin = new QDoubleSpinBox(w);
    m_genLevelSpin->setRange(-100.0, 0.0);
    m_genLevelSpin->setSuffix(" dB");
    form->addRow("Signal Level:", m_genLevelSpin);

    return w;
}

QWidget* DevicePickerView::createPbCoreAudioView() {
    auto w = new QWidget(this);
    auto form = new QFormLayout(w);

    m_pbDeviceCombo = new QComboBox(w);
    connect(m_pbDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        Q_UNUSED(idx);
        applySettings();
    });
    form->addRow("Device:", m_pbDeviceCombo);

    auto chBox = new QHBoxLayout();
    m_pbDevChannelsSpin = new QSpinBox(w);
    m_pbDevChannelsSpin->setRange(1, 32);
    chBox->addWidget(new QLabel("Device Ch:"));
    chBox->addWidget(m_pbDevChannelsSpin);

    m_pbStreamChannelsSpin = new QSpinBox(w);
    m_pbStreamChannelsSpin->setRange(1, 32);
    chBox->addWidget(new QLabel("Stream Ch:"));
    chBox->addWidget(m_pbStreamChannelsSpin);
    form->addRow("Channels:", chBox);

    m_pbRateCombo = new QComboBox(w);
    form->addRow("Sample Rate:", m_pbRateCombo);

    m_pbFormatCombo = new QComboBox(w);
    form->addRow("Sample Format:", m_pbFormatCombo);

    m_exclusiveModeCheck = new QCheckBox("Exclusive Mode (Hog Device Access)", w);
    form->addRow("", m_exclusiveModeCheck);

    m_outputDoPCheck = new QCheckBox("Output DoP (DSD-over-PCM)", w);
    form->addRow("", m_outputDoPCheck);

    m_sdmFilterCombo = new QComboBox(w);
    m_sdmFilterCombo->addItems({"SDM5", "SDM6", "SDM7"});
    form->addRow("SDM Encoder Filter:", m_sdmFilterCombo);

    return w;
}

QWidget* DevicePickerView::createPbFileView() {
    auto w = new QWidget(this);
    auto form = new QFormLayout(w);

    auto fileBox = new QHBoxLayout();
    m_pbFilePathEdit = new QLineEdit(w);
    fileBox->addWidget(m_pbFilePathEdit);

    auto browseBtn = new QPushButton("Browse...", w);
    connect(browseBtn, &QPushButton::clicked, [this, w]() {
        QString path = QFileDialog::getSaveFileName(w, "Select Output File", "", "Raw Files (*.raw *.f64 *.f32)");
        if (!path.isEmpty()) m_pbFilePathEdit->setText(path);
    });
    fileBox->addWidget(browseBtn);
    form->addRow("File Path:", fileBox);

    m_pbFileFormatCombo = new QComboBox(w);
    m_pbFileFormatCombo->addItems({"S16_LE", "S24_3_LE", "S24_4_RJ_LE", "S24_4_LJ_LE", "S32_LE", "F32_LE", "F64_LE"});
    form->addRow("Format:", m_pbFileFormatCombo);

    m_pbFileChannelsSpin = new QSpinBox(w);
    m_pbFileChannelsSpin->setRange(1, 32);
    form->addRow("Channels:", m_pbFileChannelsSpin);

    return w;
}

void DevicePickerView::refreshUi() {
    m_capDeviceCombo->blockSignals(true);
    m_pbDeviceCombo->blockSignals(true);

    m_capDeviceCombo->clear();
    m_capDeviceCombo->addItem("System Default", QString());
    for (const auto& dev : m_devices->captureDevices) {
        m_capDeviceCombo->addItem(QString::fromStdString(dev.name), QString::fromStdString(dev.name));
    }

    if (auto name = m_devices->captureConfig.deviceName()) {
        int idx = m_capDeviceCombo->findData(QString::fromStdString(name.value()));
        if (idx >= 0) m_capDeviceCombo->setCurrentIndex(idx);
    } else {
        m_capDeviceCombo->setCurrentIndex(0);
    }

    m_pbDeviceCombo->clear();
    m_pbDeviceCombo->addItem("System Default", QString());
    for (const auto& dev : m_devices->playbackDevices) {
        m_pbDeviceCombo->addItem(QString::fromStdString(dev.name), QString::fromStdString(dev.name));
    }

    if (auto name = m_devices->playbackConfig.deviceName()) {
        int idx = m_pbDeviceCombo->findData(QString::fromStdString(name.value()));
        if (idx >= 0) m_pbDeviceCombo->setCurrentIndex(idx);
    } else {
        m_pbDeviceCombo->setCurrentIndex(0);
    }

    m_capDeviceCombo->blockSignals(false);
    m_pbDeviceCombo->blockSignals(false);

    int capBackendIdx = static_cast<int>(m_devices->captureConfig.backend);
    m_capBackendCombo->setCurrentIndex(capBackendIdx);
    m_capStack->setCurrentIndex(capBackendIdx);

    int pbBackendIdx = (m_devices->playbackConfig.backend == AudioBackendType::CoreAudio) ? 0 : 1;
    m_pbBackendCombo->setCurrentIndex(pbBackendIdx);
    m_pbStack->setCurrentIndex(pbBackendIdx);

    m_capDevChannelsSpin->setValue(m_devices->captureConfig.deviceChannels);
    m_capStreamChannelsSpin->setValue(m_devices->captureConfig.channels);

    // Populate dynamic rates & formats
    m_capRateCombo->clear();
    auto capRates = m_devices->captureConfig.supportedRates();
    if (capRates.empty()) capRates = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
    for (int rate : capRates) {
        m_capRateCombo->addItem(QString("%1 Hz").arg(rate), rate);
    }
    int capRateIdx = m_capRateCombo->findData(m_devices->captureConfig.sampleRate);
    if (capRateIdx >= 0) m_capRateCombo->setCurrentIndex(capRateIdx);

    m_capFormatCombo->clear();
    auto capFormats = m_devices->captureConfig.supportedFormats();
    if (capFormats.empty()) capFormats = {"F32", "S32", "S24", "S16", "F64"};
    for (const auto& fmt : capFormats) {
        m_capFormatCombo->addItem(QString::fromStdString(fmt));
    }
    m_capFormatCombo->setCurrentText(QString::fromStdString(m_devices->captureConfig.format));
    m_bypassDoPCheck->setChecked(m_devices->captureConfig.bypassDoP);

    m_pbDevChannelsSpin->setValue(m_devices->playbackConfig.deviceChannels);
    m_pbStreamChannelsSpin->setValue(m_devices->playbackConfig.channels);

    m_pbRateCombo->clear();
    auto pbRates = m_devices->playbackConfig.supportedRates();
    if (pbRates.empty()) pbRates = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
    for (int rate : pbRates) {
        m_pbRateCombo->addItem(QString("%1 Hz").arg(rate), rate);
    }
    int pbRateIdx = m_pbRateCombo->findData(m_devices->playbackConfig.sampleRate);
    if (pbRateIdx >= 0) m_pbRateCombo->setCurrentIndex(pbRateIdx);

    m_pbFormatCombo->clear();
    auto pbFormats = m_devices->playbackConfig.supportedFormats();
    if (pbFormats.empty()) pbFormats = {"F32", "S32", "S24", "S16", "F64"};
    for (const auto& fmt : pbFormats) {
        m_pbFormatCombo->addItem(QString::fromStdString(fmt));
    }
    m_pbFormatCombo->setCurrentText(QString::fromStdString(m_devices->playbackConfig.format));
    m_exclusiveModeCheck->setChecked(m_devices->exclusiveMode);
    m_outputDoPCheck->setChecked(m_devices->playbackConfig.outputDoP);

    int chunkIdx = m_chunkSizeCombo->findData(m_settings->chunkSize);
    if (chunkIdx >= 0) m_chunkSizeCombo->setCurrentIndex(chunkIdx);
    m_enableRateAdjustCheck->setChecked(m_settings->enableRateAdjust);
}

void DevicePickerView::applySettings() {
    int sampleRate = m_capRateCombo->currentData().toInt();
    int chunkSize = m_chunkSizeCombo->currentData().toInt();

    m_settings->chunkSize = chunkSize;
    m_settings->enableRateAdjust = m_enableRateAdjustCheck->isChecked();
    m_settings->savePreferences();

    DeviceConfig capCfg = m_devices->captureConfig;
    capCfg.backend = static_cast<AudioBackendType>(m_capBackendCombo->currentIndex());
    capCfg.setDeviceName(m_capDeviceCombo->currentData().toString().toStdString());
    capCfg.deviceChannels = m_capDevChannelsSpin->value();
    capCfg.channels = m_capStreamChannelsSpin->value();
    if (sampleRate > 0) capCfg.sampleRate = sampleRate;
    if (!m_capFormatCombo->currentText().isEmpty()) capCfg.format = m_capFormatCombo->currentText().toStdString();
    capCfg.bypassDoP = m_bypassDoPCheck->isChecked();
    capCfg.filename = m_capFilePathEdit->text().toStdString();
    capCfg.generatorType = m_genTypeCombo->currentText().toStdString();
    capCfg.generatorFreq = m_genFreqSpin->value();
    capCfg.generatorLevel = m_genLevelSpin->value();
    m_devices->setCaptureConfig(capCfg);

    DeviceConfig pbCfg = m_devices->playbackConfig;
    pbCfg.backend = (m_pbBackendCombo->currentIndex() == 0) ? AudioBackendType::CoreAudio : AudioBackendType::RawFile;
    pbCfg.setDeviceName(m_pbDeviceCombo->currentData().toString().toStdString());
    pbCfg.deviceChannels = m_pbDevChannelsSpin->value();
    pbCfg.channels = m_pbStreamChannelsSpin->value();
    if (sampleRate > 0) pbCfg.sampleRate = sampleRate;
    if (!m_pbFormatCombo->currentText().isEmpty()) pbCfg.format = m_pbFormatCombo->currentText().toStdString();
    pbCfg.outputDoP = m_outputDoPCheck->isChecked();
    pbCfg.filename = m_pbFilePathEdit->text().toStdString();
    m_devices->setPlaybackConfig(pbCfg);

    m_devices->setExclusiveMode(m_exclusiveModeCheck->isChecked());
}
