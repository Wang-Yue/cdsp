#include "ui/DevicePickerView.h"

#include "ui/StyleTheme.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

namespace {

class DeviceRowWidget : public QWidget {
public:
    DeviceRowWidget(const QString& name, bool isSelected, QWidget* parent = nullptr) : QWidget(parent) {
        auto layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 4, 8, 4);

        auto leftCheck = new QLabel(isSelected ? "✓" : " ", this);
        leftCheck->setStyleSheet(isSelected ? "color: #007af5; font-weight: bold; font-size: 14px;" : "");
        leftCheck->setFixedWidth(16);
        layout->addWidget(leftCheck);

        auto textLbl = new QLabel(name, this);
        if (isSelected) {
            QFont f = textLbl->font();
            f.setBold(true);
            textLbl->setFont(f);
            textLbl->setStyleSheet("color: #007af5;");
        }
        layout->addWidget(textLbl);
        layout->addStretch();

        if (isSelected) {
            auto rightCheck = new QLabel("✓", this);
            rightCheck->setStyleSheet("color: #007af5; font-weight: bold; font-size: 12px;");
            layout->addWidget(rightCheck);
        }
    }
};

} // namespace

DevicePickerView::DevicePickerView(std::shared_ptr<AudioDeviceManager> devices, std::shared_ptr<AudioSettings> settings,
                                   QWidget* parent)
    : QWidget(parent), m_devices(devices), m_settings(settings) {
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

    auto capHeaderLabel = new QLabel("🎤  Capture Settings", capGroup);
    capHeaderLabel->setFont(QFont("sans-serif", 13, QFont::Bold));
    capHeaderLabel->setStyleSheet("color: #007af5;");
    capLayout->addWidget(capHeaderLabel);

    auto capBackendBox = new QHBoxLayout();
    auto capBackendLbl = new QLabel("Backend", capGroup);
    capBackendLbl->setFixedWidth(100);
    capBackendBox->addWidget(capBackendLbl);

    m_capBackendCombo = new QComboBox(capGroup);
#if defined(Q_OS_MAC)
    m_capBackendCombo->addItem("CoreAudio", static_cast<int>(AudioBackendType::CoreAudio));
#elif defined(Q_OS_WIN)
    m_capBackendCombo->addItem("WASAPI", static_cast<int>(AudioBackendType::WASAPI));
    m_capBackendCombo->addItem("ASIO", static_cast<int>(AudioBackendType::ASIO));
#else
    m_capBackendCombo->addItem("ALSA", static_cast<int>(AudioBackendType::ALSA));
    m_capBackendCombo->addItem("PulseAudio", static_cast<int>(AudioBackendType::PulseAudio));
#endif
    m_capBackendCombo->addItem("RawFile", static_cast<int>(AudioBackendType::RawFile));
    m_capBackendCombo->addItem("WavFile", static_cast<int>(AudioBackendType::WavFile));
    m_capBackendCombo->addItem("SignalGenerator", static_cast<int>(AudioBackendType::SignalGenerator));

    auto getCapStackIndex = [](AudioBackendType backend) {
        switch (backend) {
        case AudioBackendType::CoreAudio:
        case AudioBackendType::WASAPI:
        case AudioBackendType::ASIO:
        case AudioBackendType::ALSA:
        case AudioBackendType::PulseAudio:
            return 0;
        case AudioBackendType::RawFile:
            return 1;
        case AudioBackendType::WavFile:
            return 2;
        case AudioBackendType::SignalGenerator:
            return 3;
        }
        return 0;
    };

    connect(m_capBackendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, getCapStackIndex](int) {
        if (m_isRefreshing)
            return;
        AudioBackendType b = static_cast<AudioBackendType>(m_capBackendCombo->currentData().toInt());
        m_capStack->setCurrentIndex(getCapStackIndex(b));
        applySettings();
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

    auto pbHeaderLabel = new QLabel("🔊  Playback Settings", pbGroup);
    pbHeaderLabel->setFont(QFont("sans-serif", 13, QFont::Bold));
    pbHeaderLabel->setStyleSheet("color: #34c759;");
    pbLayout->addWidget(pbHeaderLabel);

    auto pbBackendBox = new QHBoxLayout();
    auto pbBackendLbl = new QLabel("Backend", pbGroup);
    pbBackendLbl->setFixedWidth(100);
    pbBackendBox->addWidget(pbBackendLbl);

    m_pbBackendCombo = new QComboBox(pbGroup);
#if defined(Q_OS_MAC)
    m_pbBackendCombo->addItem("CoreAudio", static_cast<int>(AudioBackendType::CoreAudio));
#elif defined(Q_OS_WIN)
    m_pbBackendCombo->addItem("WASAPI", static_cast<int>(AudioBackendType::WASAPI));
    m_pbBackendCombo->addItem("ASIO", static_cast<int>(AudioBackendType::ASIO));
#else
    m_pbBackendCombo->addItem("ALSA", static_cast<int>(AudioBackendType::ALSA));
    m_pbBackendCombo->addItem("PulseAudio", static_cast<int>(AudioBackendType::PulseAudio));
#endif
    m_pbBackendCombo->addItem("RawFile", static_cast<int>(AudioBackendType::RawFile));

    auto getPbStackIndex = [](AudioBackendType backend) {
        switch (backend) {
        case AudioBackendType::CoreAudio:
        case AudioBackendType::WASAPI:
        case AudioBackendType::ASIO:
        case AudioBackendType::ALSA:
        case AudioBackendType::PulseAudio:
            return 0;
        case AudioBackendType::RawFile:
        case AudioBackendType::WavFile:
        case AudioBackendType::SignalGenerator:
            return 1;
        }
        return 0;
    };

    connect(m_pbBackendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, getPbStackIndex](int) {
        if (m_isRefreshing)
            return;
        AudioBackendType b = static_cast<AudioBackendType>(m_pbBackendCombo->currentData().toInt());
        m_pbStack->setCurrentIndex(getPbStackIndex(b));
        applySettings();
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
    auto procLayout = new QVBoxLayout(procGroup);

    auto procHeaderLabel = new QLabel("⚙️  Processing Settings", procGroup);
    procHeaderLabel->setFont(QFont("sans-serif", 13, QFont::Bold));
    procLayout->addWidget(procHeaderLabel);

    auto procForm = new QFormLayout();
    procForm->setSpacing(12);

    auto chunkLayout = new QHBoxLayout();
    m_chunkSizeCombo = new QComboBox(procGroup);
    for (int size : {256, 512, 1024, 2048, 4096, 8192, 16384, 32768}) {
        m_chunkSizeCombo->addItem(QString("%1 samples").arg(size), size);
    }
    chunkLayout->addWidget(m_chunkSizeCombo);

    m_latencyLabel = new QLabel(procGroup);
    m_latencyLabel->setStyleSheet("color: #8e8e93; font-style: italic;");
    connect(m_chunkSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        if (m_chunkSizeCombo && m_settings) {
            m_settings->chunkSize = m_chunkSizeCombo->currentData().toInt();
        }
        updateLatencyText();
        applySettings();
    });
    chunkLayout->addWidget(m_latencyLabel);
    chunkLayout->addStretch();
    procForm->addRow("Chunk Size", chunkLayout);

    m_enableRateAdjustCheck = new QCheckBox("Enable Rate Adjust", procGroup);
    connect(m_enableRateAdjustCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    procForm->addRow("", m_enableRateAdjustCheck);

    m_rateAdjustSub = new QLabel("Compensate for clock drift between capture and playback devices", procGroup);
    m_rateAdjustSub->setStyleSheet("color: #8e8e93; font-size: 11px;");
    procForm->addRow("", m_rateAdjustSub);

    m_queueLimitSpin = new QSpinBox(procGroup);
    m_queueLimitSpin->setRange(1, 32);
    m_queueLimitSpin->setFixedWidth(100);
    connect(m_queueLimitSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    procForm->addRow("Queue Limit", m_queueLimitSpin);

    m_stopOnRateChangeCheck = new QCheckBox("Stop on Rate Change", procGroup);
    connect(m_stopOnRateChangeCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    procForm->addRow("", m_stopOnRateChangeCheck);

    auto intervalBox = new QHBoxLayout();
    m_measureIntervalSpin = new QDoubleSpinBox(procGroup);
    m_measureIntervalSpin->setRange(0.1, 10.0);
    m_measureIntervalSpin->setSingleStep(0.1);
    m_measureIntervalSpin->setSuffix(" s");
    m_measureIntervalSpin->setFixedWidth(90);

    m_measureIntervalSlider = new QSlider(Qt::Horizontal, procGroup);
    m_measureIntervalSlider->setRange(1, 100); // 0.1 to 10.0 s
    m_measureIntervalSlider->setFixedWidth(150);

    m_measureIntervalValLabel = new QLabel(procGroup);
    m_measureIntervalValLabel->setStyleSheet("font-family: monospace; font-size: 13px;");

    connect(m_measureIntervalSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this](double val) {
        if (m_isRefreshing)
            return;
        m_measureIntervalSlider->blockSignals(true);
        m_measureIntervalSlider->setValue(static_cast<int>(val * 10.0));
        m_measureIntervalSlider->blockSignals(false);
        m_measureIntervalValLabel->setText(QString("%1 s").arg(val, 0, 'f', 1));
        applySettings();
    });
    connect(m_measureIntervalSlider, &QSlider::valueChanged, [this](int val) {
        if (m_isRefreshing)
            return;
        double dVal = val / 10.0;
        m_measureIntervalSpin->blockSignals(true);
        m_measureIntervalSpin->setValue(dVal);
        m_measureIntervalSpin->blockSignals(false);
        m_measureIntervalValLabel->setText(QString("%1 s").arg(dVal, 0, 'f', 1));
        applySettings();
    });
    intervalBox->addWidget(m_measureIntervalSpin);
    intervalBox->addWidget(m_measureIntervalSlider);
    intervalBox->addWidget(m_measureIntervalValLabel);
    intervalBox->addStretch();
    procForm->addRow("Measure Interval", intervalBox);

    m_multithreadedCheck = new QCheckBox("Multithreaded", procGroup);
    connect(m_multithreadedCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_isRefreshing)
            return;
        m_workerThreadsRow->setVisible(checked);
        applySettings();
    });
    procForm->addRow("", m_multithreadedCheck);

    m_workerThreadsRow = new QWidget(procGroup);
    auto wtLayout = new QHBoxLayout(m_workerThreadsRow);
    wtLayout->setContentsMargins(16, 0, 0, 0);

    auto wtLbl = new QLabel("Worker Threads", m_workerThreadsRow);
    wtLbl->setFixedWidth(120);
    wtLayout->addWidget(wtLbl);

    m_workerThreadsSpin = new QSpinBox(m_workerThreadsRow);
    m_workerThreadsSpin->setRange(0, 32);
    m_workerThreadsSpin->setSpecialValueText("Auto");
    m_workerThreadsSpin->setFixedWidth(100);
    connect(m_workerThreadsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    wtLayout->addWidget(m_workerThreadsSpin);
    wtLayout->addStretch();
    procForm->addRow("", m_workerThreadsRow);

    procLayout->addLayout(procForm);
    mainLayout->addWidget(procGroup);

    // Bottom Action Buttons
    auto btnBox = new QHBoxLayout();
    auto refreshBtn = new QPushButton("Refresh Devices", container);
    connect(refreshBtn, &QPushButton::clicked, [this]() { m_devices->fetchDevices(); });
    btnBox->addWidget(refreshBtn);

    btnBox->addStretch();

    auto applyBtn = new QPushButton("Apply Hardware Settings", container);
    applyBtn->setStyleSheet(
        "background-color: #007af5; color: white; font-weight: bold; padding: 6px 16px; border-radius: 4px;");
    connect(applyBtn, &QPushButton::clicked, this, &DevicePickerView::applySettings);
    btnBox->addWidget(applyBtn);

    mainLayout->addLayout(btnBox);
    scroll->setWidget(container);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

QString DevicePickerView::formatSampleRate(int rate) {
    if (rate >= 1000) {
        return QString("%1 kHz").arg(rate / 1000.0, 0, 'f', 1);
    }
    return QString("%1 Hz").arg(rate);
}

void DevicePickerView::populateDeviceCombo(QComboBox* combo, QWidget* warningWidget,
                                           const std::vector<AudioDevice>& devices,
                                           const std::optional<std::string>& selectedDeviceName) {
    combo->blockSignals(true);
    combo->clear();

    if (devices.empty()) {
        warningWidget->show();
        combo->hide();
        combo->blockSignals(false);
        return;
    }

    warningWidget->hide();
    combo->show();

    combo->addItem("System Default", QString(""));

    int selectedIdx = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& dev = devices[i];
        QString devName = QString::fromStdString(dev.name);
        combo->addItem(devName, devName);
        if (selectedDeviceName.has_value() && selectedDeviceName.value() == dev.name) {
            selectedIdx = static_cast<int>(i + 1);
        }
    }

    combo->setCurrentIndex(selectedIdx);
    combo->blockSignals(false);
}

QWidget* DevicePickerView::createCapCoreAudioView() {
    auto w = new QWidget();
    auto form = new QFormLayout(w);
    form->setSpacing(12);

    m_capWarningWidget = new QWidget(w);
    auto warnLayout = new QHBoxLayout(m_capWarningWidget);
    warnLayout->setContentsMargins(0, 4, 0, 4);
    auto warnIcon = new QLabel("⚠️", m_capWarningWidget);
    warnIcon->setStyleSheet("font-size: 16px; color: #ff9500;");
    auto warnText = new QLabel("No devices found", m_capWarningWidget);
    warnText->setStyleSheet("color: #8e8e93; font-weight: 500;");
    warnLayout->addWidget(warnIcon);
    warnLayout->addWidget(warnText);
    warnLayout->addStretch();

    m_capDeviceCombo = new QComboBox(w);
    m_capDeviceCombo->setMinimumWidth(260);
    connect(m_capDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (m_isRefreshing || idx < 0)
            return;
        std::string newDev = m_capDeviceCombo->currentData().toString().toStdString();
        m_devices->captureConfig.setDeviceName(newDev);
        m_devices->refreshDeviceCapabilities();
        m_devices->validateSampleRates();
        applySettings();
        refreshUi();
    });

    auto devBox = new QVBoxLayout();
    devBox->addWidget(m_capWarningWidget);
    devBox->addWidget(m_capDeviceCombo);
    form->addRow("Device Selection", devBox);

    auto chBox = new QHBoxLayout();
    m_capDevChannelsCombo = new QComboBox(w);
    m_capDevChannelsSpin = new QSpinBox(w);
    m_capDevChannelsSpin->setRange(1, 32);
    m_capDevChannelsSpin->setFixedWidth(100);

    auto onDevChChanged = [this](int ch) {
        if (m_isRefreshing)
            return;
        m_capStreamChannelsSpin->setMaximum(ch);
        if (m_capStreamChannelsSpin->value() > ch) {
            m_capStreamChannelsSpin->setValue(ch);
        }
        applySettings();
    };
    connect(m_capDevChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), onDevChChanged);
    connect(m_capDevChannelsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, onDevChChanged](int) {
        if (!m_isRefreshing && m_capDevChannelsCombo->currentIndex() >= 0) {
            onDevChChanged(m_capDevChannelsCombo->currentData().toInt());
        }
    });

    auto devChLbl = new QLabel("Device Channels", w);
    devChLbl->setFixedWidth(110);
    chBox->addWidget(devChLbl);
    chBox->addWidget(m_capDevChannelsCombo);
    chBox->addWidget(m_capDevChannelsSpin);

    m_capStreamChannelsSpin = new QSpinBox(w);
    m_capStreamChannelsSpin->setRange(1, 32);
    m_capStreamChannelsSpin->setFixedWidth(100);
    connect(m_capStreamChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });

    auto streamChLbl = new QLabel("Stream Channels", w);
    streamChLbl->setFixedWidth(110);
    chBox->addWidget(streamChLbl);
    chBox->addWidget(m_capStreamChannelsSpin);
    chBox->addStretch();
    form->addRow("", chBox);

    auto rateBox = new QHBoxLayout();
    auto rateLbl = new QLabel("Sample Rate", w);
    rateLbl->setFixedWidth(100);
    rateBox->addWidget(rateLbl);

    m_capRateCombo = new QComboBox(w);
    connect(m_capRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });

    m_capRateLabel = new QLabel(w);
    m_capRateLabel->setStyleSheet("font-family: monospace; color: #8e8e93; font-size: 13px;");

    rateBox->addWidget(m_capRateCombo);
    rateBox->addWidget(m_capRateLabel);
    rateBox->addStretch();
    form->addRow("", rateBox);

    auto fmtBox = new QHBoxLayout();
    auto fmtLbl = new QLabel("Format", w);
    fmtLbl->setFixedWidth(100);
    fmtBox->addWidget(fmtLbl);

    m_capFormatCombo = new QComboBox(w);
    connect(m_capFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });

    m_capFormatLabel = new QLabel(w);
    m_capFormatLabel->setStyleSheet("font-family: monospace; color: #8e8e93; font-size: 13px;");

    fmtBox->addWidget(m_capFormatCombo);
    fmtBox->addWidget(m_capFormatLabel);
    fmtBox->addStretch();
    form->addRow("", fmtBox);

    m_bypassDoPCheck = new QCheckBox("Bypass DoP Detection", w);
    connect(m_bypassDoPCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_isRefreshing)
            return;
        m_dopCutoffLabel->setEnabled(!checked);
        m_dopCutoffCombo->setEnabled(!checked);
        applySettings();
    });
    form->addRow("", m_bypassDoPCheck);

    auto cutoffBox = new QHBoxLayout();
    m_dopCutoffLabel = new QLabel("DoP Cutoff", w);
    m_dopCutoffLabel->setFixedWidth(100);
    cutoffBox->addWidget(m_dopCutoffLabel);

    m_dopCutoffCombo = new QComboBox(w);
    m_dopCutoffCombo->addItem("20 kHz", 20000.0);
    m_dopCutoffCombo->addItem("25 kHz", 25000.0);
    m_dopCutoffCombo->addItem("30 kHz", 30000.0);
    m_dopCutoffCombo->addItem("40 kHz", 40000.0);
    m_dopCutoffCombo->addItem("50 kHz", 50000.0);
    connect(m_dopCutoffCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    cutoffBox->addWidget(m_dopCutoffCombo);
    cutoffBox->addStretch();
    form->addRow("", cutoffBox);

    m_dopCutoffHint = new QLabel("Lower cutoff = higher SINAD; higher cutoff preserves more ultrasonic content", w);
    m_dopCutoffHint->setStyleSheet("color: #8e8e93; font-size: 11px;");
    form->addRow("", m_dopCutoffHint);

    return w;
}

QWidget* DevicePickerView::createCapFileView(bool isWav) {
    auto w = new QWidget();
    auto form = new QFormLayout(w);
    form->setSpacing(12);

    auto fileBox = new QHBoxLayout();
    auto pathLbl = new QLabel("File Path", w);
    pathLbl->setFixedWidth(100);
    fileBox->addWidget(pathLbl);

    if (isWav) {
        m_capWavFilePathEdit = new QLineEdit(w);
        m_capWavFilePathEdit->setPlaceholderText("e.g. /path/to/audio.wav");
        connect(m_capWavFilePathEdit, &QLineEdit::textChanged, [this](const QString&) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        fileBox->addWidget(m_capWavFilePathEdit);

        auto browseBtn = new QPushButton("Browse...", w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path = QFileDialog::getOpenFileName(w, "Select WAV File", "", "WAV Files (*.wav)");
            if (!path.isEmpty())
                m_capWavFilePathEdit->setText(path);
        });
        fileBox->addWidget(browseBtn);
        form->addRow("", fileBox);

        auto noteLbl = new QLabel("Sample rate, format, and channel count are parsed from the file header", w);
        noteLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        form->addRow("", noteLbl);

        m_capWavSkipBytesSpin = new QSpinBox(w);
        m_capWavSkipBytesSpin->setRange(0, 1000000);
        m_capWavSkipBytesSpin->setFixedWidth(100);
        connect(m_capWavSkipBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Skip Bytes", m_capWavSkipBytesSpin);

        m_capWavReadBytesSpin = new QSpinBox(w);
        m_capWavReadBytesSpin->setRange(0, 100000000);
        m_capWavReadBytesSpin->setSpecialValueText("0 (All)");
        m_capWavReadBytesSpin->setFixedWidth(100);
        connect(m_capWavReadBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Read Bytes", m_capWavReadBytesSpin);

        m_capWavExtraSamplesSpin = new QSpinBox(w);
        m_capWavExtraSamplesSpin->setRange(0, 1000000);
        m_capWavExtraSamplesSpin->setFixedWidth(120);
        connect(m_capWavExtraSamplesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Extra Samples", m_capWavExtraSamplesSpin);

    } else {
        m_capRawFilePathEdit = new QLineEdit(w);
        m_capRawFilePathEdit->setPlaceholderText("e.g. /path/to/audio.raw");
        connect(m_capRawFilePathEdit, &QLineEdit::textChanged, [this](const QString&) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        fileBox->addWidget(m_capRawFilePathEdit);

        auto browseBtn = new QPushButton("Browse...", w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path = QFileDialog::getOpenFileName(w, "Select Raw File", "", "Raw Files (*.raw *.f64 *.f32)");
            if (!path.isEmpty())
                m_capRawFilePathEdit->setText(path);
        });
        fileBox->addWidget(browseBtn);
        form->addRow("", fileBox);

        m_capRawFileFormatCombo = new QComboBox(w);
        m_capRawFileFormatCombo->addItems(
            {"S16_LE", "S24_3_LE", "S24_4_RJ_LE", "S24_4_LJ_LE", "S32_LE", "F32_LE", "F64_LE"});
        connect(m_capRawFileFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Format", m_capRawFileFormatCombo);

        m_capRawFileChannelsSpin = new QSpinBox(w);
        m_capRawFileChannelsSpin->setRange(1, 32);
        m_capRawFileChannelsSpin->setFixedWidth(100);
        connect(m_capRawFileChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Channels", m_capRawFileChannelsSpin);

        m_capRawSkipBytesSpin = new QSpinBox(w);
        m_capRawSkipBytesSpin->setRange(0, 1000000);
        m_capRawSkipBytesSpin->setFixedWidth(100);
        connect(m_capRawSkipBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Skip Bytes", m_capRawSkipBytesSpin);

        m_capRawReadBytesSpin = new QSpinBox(w);
        m_capRawReadBytesSpin->setRange(0, 100000000);
        m_capRawReadBytesSpin->setSpecialValueText("0 (All)");
        m_capRawReadBytesSpin->setFixedWidth(100);
        connect(m_capRawReadBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Read Bytes", m_capRawReadBytesSpin);

        m_capRawExtraSamplesSpin = new QSpinBox(w);
        m_capRawExtraSamplesSpin->setRange(0, 1000000);
        m_capRawExtraSamplesSpin->setFixedWidth(120);
        connect(m_capRawExtraSamplesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Extra Samples", m_capRawExtraSamplesSpin);
    }

    return w;
}

QWidget* DevicePickerView::createCapGeneratorView() {
    auto w = new QWidget();
    auto form = new QFormLayout(w);
    form->setSpacing(12);

    m_genTypeCombo = new QComboBox(w);
    m_genTypeCombo->addItems({"Sine", "Square", "WhiteNoise"});
    connect(m_genTypeCombo, &QComboBox::currentTextChanged, [this](const QString& type) {
        bool isNoise = (type == "WhiteNoise");
        m_genFreqLabel->setEnabled(!isNoise);
        m_genFreqSpin->setEnabled(!isNoise);
        m_genFreqSlider->setEnabled(!isNoise);
        if (m_isRefreshing)
            return;
        applySettings();
    });
    form->addRow("Generator", m_genTypeCombo);

    m_genChannelsSpin = new QSpinBox(w);
    m_genChannelsSpin->setRange(1, 32);
    m_genChannelsSpin->setFixedWidth(100);
    connect(m_genChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    form->addRow("Channels", m_genChannelsSpin);

    auto freqBox = new QHBoxLayout();
    m_genFreqLabel = new QLabel("Freq (Hz)", w);
    m_genFreqLabel->setFixedWidth(100);

    m_genFreqSpin = new QDoubleSpinBox(w);
    m_genFreqSpin->setRange(1.0, 20000.0);
    m_genFreqSpin->setSingleStep(1.0);
    m_genFreqSpin->setSuffix(" Hz");
    m_genFreqSpin->setFixedWidth(100);

    m_genFreqSlider = new QSlider(Qt::Horizontal, w);
    m_genFreqSlider->setRange(1, 20000);

    connect(m_genFreqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this](double val) {
        if (m_isRefreshing)
            return;
        m_genFreqSlider->blockSignals(true);
        m_genFreqSlider->setValue(static_cast<int>(val));
        m_genFreqSlider->blockSignals(false);
        applySettings();
    });
    connect(m_genFreqSlider, &QSlider::valueChanged, [this](int val) {
        if (m_isRefreshing)
            return;
        m_genFreqSpin->blockSignals(true);
        m_genFreqSpin->setValue(val);
        m_genFreqSpin->blockSignals(false);
        applySettings();
    });

    freqBox->addWidget(m_genFreqSpin);
    freqBox->addWidget(m_genFreqSlider);
    form->addRow(m_genFreqLabel, freqBox);

    auto levelBox = new QHBoxLayout();
    auto levelLbl = new QLabel("Level (dB)", w);
    levelLbl->setFixedWidth(100);

    m_genLevelSpin = new QDoubleSpinBox(w);
    m_genLevelSpin->setRange(-100.0, 0.0);
    m_genLevelSpin->setSingleStep(0.5);
    m_genLevelSpin->setSuffix(" dB");
    m_genLevelSpin->setFixedWidth(100);

    m_genLevelSlider = new QSlider(Qt::Horizontal, w);
    m_genLevelSlider->setRange(-200, 0); // maps -100.0 to 0.0 dB

    connect(m_genLevelSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this](double val) {
        if (m_isRefreshing)
            return;
        m_genLevelSlider->blockSignals(true);
        m_genLevelSlider->setValue(static_cast<int>(val * 2.0));
        m_genLevelSlider->blockSignals(false);
        applySettings();
    });
    connect(m_genLevelSlider, &QSlider::valueChanged, [this](int val) {
        if (m_isRefreshing)
            return;
        double dbVal = val / 2.0;
        m_genLevelSpin->blockSignals(true);
        m_genLevelSpin->setValue(dbVal);
        m_genLevelSpin->blockSignals(false);
        applySettings();
    });

    levelBox->addWidget(m_genLevelSpin);
    levelBox->addWidget(m_genLevelSlider);
    form->addRow(levelLbl, levelBox);

    return w;
}

QWidget* DevicePickerView::createPbCoreAudioView() {
    auto w = new QWidget();
    auto form = new QFormLayout(w);
    form->setSpacing(12);

    m_pbWarningWidget = new QWidget(w);
    auto warnLayout = new QHBoxLayout(m_pbWarningWidget);
    warnLayout->setContentsMargins(0, 4, 0, 4);
    auto warnIcon = new QLabel("⚠️", m_pbWarningWidget);
    warnIcon->setStyleSheet("font-size: 16px; color: #ff9500;");
    auto warnText = new QLabel("No devices found", m_pbWarningWidget);
    warnText->setStyleSheet("color: #8e8e93; font-weight: 500;");
    warnLayout->addWidget(warnIcon);
    warnLayout->addWidget(warnText);
    warnLayout->addStretch();

    m_pbDeviceCombo = new QComboBox(w);
    m_pbDeviceCombo->setMinimumWidth(260);
    connect(m_pbDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        if (m_isRefreshing || idx < 0)
            return;
        std::string newDev = m_pbDeviceCombo->currentData().toString().toStdString();
        m_devices->playbackConfig.setDeviceName(newDev);
        m_devices->refreshDeviceCapabilities();
        m_devices->validateSampleRates();
        applySettings();
        refreshUi();
    });

    auto devBox = new QVBoxLayout();
    devBox->addWidget(m_pbWarningWidget);
    devBox->addWidget(m_pbDeviceCombo);
    form->addRow("Device Selection", devBox);

    auto chBox = new QHBoxLayout();
    m_pbDevChannelsCombo = new QComboBox(w);
    m_pbDevChannelsSpin = new QSpinBox(w);
    m_pbDevChannelsSpin->setRange(1, 32);
    m_pbDevChannelsSpin->setFixedWidth(100);

    auto onDevChChanged = [this](int ch) {
        if (m_isRefreshing)
            return;
        m_pbStreamChannelsSpin->setMaximum(ch);
        if (m_pbStreamChannelsSpin->value() > ch) {
            m_pbStreamChannelsSpin->setValue(ch);
        }
        applySettings();
    };
    connect(m_pbDevChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), onDevChChanged);
    connect(m_pbDevChannelsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, onDevChChanged](int) {
        if (!m_isRefreshing && m_pbDevChannelsCombo->currentIndex() >= 0) {
            onDevChChanged(m_pbDevChannelsCombo->currentData().toInt());
        }
    });

    auto devChLbl = new QLabel("Device Channels", w);
    devChLbl->setFixedWidth(110);
    chBox->addWidget(devChLbl);
    chBox->addWidget(m_pbDevChannelsCombo);
    chBox->addWidget(m_pbDevChannelsSpin);

    m_pbStreamChannelsSpin = new QSpinBox(w);
    m_pbStreamChannelsSpin->setRange(1, 32);
    m_pbStreamChannelsSpin->setFixedWidth(100);
    connect(m_pbStreamChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });

    auto streamChLbl = new QLabel("Stream Channels", w);
    streamChLbl->setFixedWidth(110);
    chBox->addWidget(streamChLbl);
    chBox->addWidget(m_pbStreamChannelsSpin);
    chBox->addStretch();
    form->addRow("", chBox);

    auto rateBox = new QHBoxLayout();
    auto rateLbl = new QLabel("Sample Rate", w);
    rateLbl->setFixedWidth(100);
    rateBox->addWidget(rateLbl);

    m_pbRateCombo = new QComboBox(w);
    connect(m_pbRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
        updateDoPCapability();
    });
    rateBox->addWidget(m_pbRateCombo);
    rateBox->addStretch();
    form->addRow("", rateBox);

    auto fmtBox = new QHBoxLayout();
    auto fmtLbl = new QLabel("Format", w);
    fmtLbl->setFixedWidth(100);
    fmtBox->addWidget(fmtLbl);

    m_pbFormatCombo = new QComboBox(w);
    connect(m_pbFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });

    m_pbFormatLabel = new QLabel(w);
    m_pbFormatLabel->setStyleSheet("font-family: monospace; color: #8e8e93; font-size: 13px;");

    fmtBox->addWidget(m_pbFormatCombo);
    fmtBox->addWidget(m_pbFormatLabel);
    fmtBox->addStretch();
    form->addRow("", fmtBox);

    m_exclusiveModeCheck = new QCheckBox("Exclusive Mode (Hog)", w);
    connect(m_exclusiveModeCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    form->addRow("", m_exclusiveModeCheck);

    m_exclusiveModeHint =
        new QLabel("Takes exclusive access to the output device, preventing other apps from using it", w);
    m_exclusiveModeHint->setStyleSheet("color: #8e8e93; font-size: 11px;");
    form->addRow("", m_exclusiveModeHint);

    m_outputDoPCheck = new QCheckBox("Output DoP (DSD-over-PCM)", w);
    connect(m_outputDoPCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
        updateDoPCapability();
    });
    form->addRow("", m_outputDoPCheck);

    auto sdmBox = new QHBoxLayout();
    m_sdmFilterLabel = new QLabel("SDM Filter", w);
    m_sdmFilterLabel->setFixedWidth(100);
    sdmBox->addWidget(m_sdmFilterLabel);

    m_sdmFilterCombo = new QComboBox(w);
    m_sdmFilterCombo->addItem("clans-4", static_cast<int>(SDMFilter::Clans4));
    m_sdmFilterCombo->addItem("sdm-4", static_cast<int>(SDMFilter::SDM4));
    m_sdmFilterCombo->addItem("clans-5", static_cast<int>(SDMFilter::Clans5));
    m_sdmFilterCombo->addItem("sdm-5", static_cast<int>(SDMFilter::SDM5));
    m_sdmFilterCombo->addItem("clans-6", static_cast<int>(SDMFilter::Clans6));
    m_sdmFilterCombo->addItem("sdm-6", static_cast<int>(SDMFilter::SDM6));
    m_sdmFilterCombo->addItem("clans-7", static_cast<int>(SDMFilter::Clans7));
    m_sdmFilterCombo->addItem("sdm-7", static_cast<int>(SDMFilter::SDM7));
    m_sdmFilterCombo->addItem("clans-8", static_cast<int>(SDMFilter::Clans8));
    m_sdmFilterCombo->addItem("sdm-8", static_cast<int>(SDMFilter::SDM8));
    connect(m_sdmFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    sdmBox->addWidget(m_sdmFilterCombo);
    sdmBox->addStretch();
    form->addRow("", sdmBox);

    m_pbDopHintLabel = new QLabel("Sample rate must be a DSD carrier rate to enable DoP output", w);
    m_pbDopHintLabel->setStyleSheet("color: #8e8e93; font-size: 11px;");
    form->addRow("", m_pbDopHintLabel);

    return w;
}

void DevicePickerView::updateDoPCapability() {
    if (!m_pbRateCombo || !m_outputDoPCheck || !m_sdmFilterCombo || !m_pbDopHintLabel)
        return;
    int currentRate = m_pbRateCombo->currentData().toInt();
    bool isCapable = (currentRate == 176400 || currentRate == 192000 || currentRate == 352800 ||
                      currentRate == 384000 || currentRate == 705600 || currentRate == 768000);
    m_outputDoPCheck->setEnabled(isCapable);
    bool sdmEnabled = isCapable && m_outputDoPCheck->isChecked();
    m_sdmFilterLabel->setEnabled(sdmEnabled);
    m_sdmFilterCombo->setEnabled(sdmEnabled);
    m_pbDopHintLabel->setVisible(!isCapable);
}

QWidget* DevicePickerView::createPbFileView() {
    auto w = new QWidget();
    auto form = new QFormLayout(w);
    form->setSpacing(12);

    auto fileBox = new QHBoxLayout();
    auto pathLbl = new QLabel("File Path", w);
    pathLbl->setFixedWidth(100);
    fileBox->addWidget(pathLbl);

    m_pbRawFilePathEdit = new QLineEdit(w);
    m_pbRawFilePathEdit->setPlaceholderText("e.g. /path/to/audio.raw");
    connect(m_pbRawFilePathEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    fileBox->addWidget(m_pbRawFilePathEdit);

    auto browseBtn = new QPushButton("Browse...", w);
    connect(browseBtn, &QPushButton::clicked, [this, w]() {
        QString path = QFileDialog::getSaveFileName(w, "Select Output File", "", "Raw Files (*.raw *.f64 *.f32)");
        if (!path.isEmpty())
            m_pbRawFilePathEdit->setText(path);
    });
    fileBox->addWidget(browseBtn);
    form->addRow("", fileBox);

    m_pbRawFileFormatCombo = new QComboBox(w);
    m_pbRawFileFormatCombo->addItems(
        {"S16_LE", "S24_3_LE", "S24_4_RJ_LE", "S24_4_LJ_LE", "S32_LE", "F32_LE", "F64_LE"});
    connect(m_pbRawFileFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    form->addRow("Format", m_pbRawFileFormatCombo);

    m_pbRawFileChannelsSpin = new QSpinBox(w);
    m_pbRawFileChannelsSpin->setRange(1, 32);
    m_pbRawFileChannelsSpin->setFixedWidth(100);
    connect(m_pbRawFileChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    form->addRow("Channels", m_pbRawFileChannelsSpin);

    return w;
}

void DevicePickerView::refreshUi() {
    m_isRefreshing = true;

    // 1. Refresh Capture Devices List & CoreAudio controls
    populateDeviceCombo(m_capDeviceCombo, m_capWarningWidget, m_devices->captureDevices,
                        m_devices->captureConfig.deviceName());

    int capBackendIdx = m_capBackendCombo->findData(static_cast<int>(m_devices->captureConfig.backend));
    if (capBackendIdx >= 0) {
        m_capBackendCombo->setCurrentIndex(capBackendIdx);
        int stackIdx = 0;
        switch (m_devices->captureConfig.backend) {
        case AudioBackendType::CoreAudio:
        case AudioBackendType::WASAPI:
        case AudioBackendType::ASIO:
        case AudioBackendType::ALSA:
        case AudioBackendType::PulseAudio:
            stackIdx = 0;
            break;
        case AudioBackendType::RawFile:
            stackIdx = 1;
            break;
        case AudioBackendType::WavFile:
            stackIdx = 2;
            break;
        case AudioBackendType::SignalGenerator:
            stackIdx = 3;
            break;
        }
        m_capStack->setCurrentIndex(stackIdx);
    }

    // Capture Channels (Device Channels combo vs spinbox)
    auto capSuppCh = m_devices->captureConfig.supportedChannels();
    if (!capSuppCh.empty()) {
        m_capDevChannelsCombo->show();
        m_capDevChannelsSpin->hide();
        m_capDevChannelsCombo->blockSignals(true);
        m_capDevChannelsCombo->clear();
        for (int ch : capSuppCh) {
            m_capDevChannelsCombo->addItem(QString::number(ch), ch);
        }
        int chIdx = m_capDevChannelsCombo->findData(m_devices->captureConfig.deviceChannels);
        if (chIdx >= 0)
            m_capDevChannelsCombo->setCurrentIndex(chIdx);
        m_capDevChannelsCombo->blockSignals(false);
    } else {
        m_capDevChannelsCombo->hide();
        m_capDevChannelsSpin->show();
        m_capDevChannelsSpin->setValue(m_devices->captureConfig.deviceChannels);
    }

    int capDevCh = m_devices->captureConfig.deviceChannels;
    m_capStreamChannelsSpin->setRange(1, std::max(1, capDevCh));
    m_capStreamChannelsSpin->setValue(m_devices->captureConfig.channels);

    // Capture Sample Rate
    if (m_settings->resamplerEnabled) {
        m_capRateCombo->show();
        m_capRateLabel->hide();
        m_capRateCombo->blockSignals(true);
        m_capRateCombo->clear();
        auto capRates = m_devices->captureRateOptions();
        for (int r : capRates) {
            m_capRateCombo->addItem(formatSampleRate(r), r);
        }
        int rIdx = m_capRateCombo->findData(m_devices->captureConfig.sampleRate);
        if (rIdx >= 0)
            m_capRateCombo->setCurrentIndex(rIdx);
        m_capRateCombo->blockSignals(false);
    } else {
        m_capRateCombo->hide();
        m_capRateLabel->show();
        m_capRateLabel->setText(formatSampleRate(m_devices->captureConfig.sampleRate));
    }

    // Capture Sample Format
    auto capFormats = m_devices->captureConfig.supportedFormats();
    if (!capFormats.empty()) {
        m_capFormatCombo->show();
        m_capFormatLabel->hide();
        m_capFormatCombo->blockSignals(true);
        m_capFormatCombo->clear();
        for (const auto& fmt : capFormats) {
            m_capFormatCombo->addItem(QString::fromStdString(fmt));
        }
        m_capFormatCombo->setCurrentText(QString::fromStdString(m_devices->captureConfig.format));
        m_capFormatCombo->blockSignals(false);
    } else {
        m_capFormatCombo->hide();
        m_capFormatLabel->show();
        m_capFormatLabel->setText(QString::fromStdString(m_devices->captureConfig.format));
    }

    // Capture DoP
    m_bypassDoPCheck->setChecked(m_devices->captureConfig.bypassDoP);
    m_dopCutoffLabel->setEnabled(!m_devices->captureConfig.bypassDoP);
    m_dopCutoffCombo->setEnabled(!m_devices->captureConfig.bypassDoP);
    int cutoffIdx = m_dopCutoffCombo->findData(m_devices->captureConfig.dopCutoffHz);
    if (cutoffIdx >= 0)
        m_dopCutoffCombo->setCurrentIndex(cutoffIdx);

    // 2. Refresh Capture File & Generator Views
    m_capRawFilePathEdit->setText(QString::fromStdString(m_devices->captureConfig.filename));
    m_capRawFileFormatCombo->setCurrentText(QString::fromStdString(m_devices->captureConfig.fileFormat));
    m_capRawFileChannelsSpin->setValue(m_devices->captureConfig.channels);
    m_capRawSkipBytesSpin->setValue(m_devices->captureConfig.skipBytes);
    m_capRawReadBytesSpin->setValue(m_devices->captureConfig.readBytes);
    m_capRawExtraSamplesSpin->setValue(m_devices->captureConfig.extraSamples);

    m_capWavFilePathEdit->setText(QString::fromStdString(m_devices->captureConfig.filename));
    m_capWavSkipBytesSpin->setValue(m_devices->captureConfig.skipBytes);
    m_capWavReadBytesSpin->setValue(m_devices->captureConfig.readBytes);
    m_capWavExtraSamplesSpin->setValue(m_devices->captureConfig.extraSamples);

    m_genTypeCombo->setCurrentText(QString::fromStdString(m_devices->captureConfig.generatorType));
    m_genChannelsSpin->setValue(m_devices->captureConfig.channels);
    m_genFreqSpin->setValue(m_devices->captureConfig.generatorFreq);
    m_genFreqSlider->setValue(static_cast<int>(m_devices->captureConfig.generatorFreq));
    m_genLevelSpin->setValue(m_devices->captureConfig.generatorLevel);
    m_genLevelSlider->setValue(static_cast<int>(m_devices->captureConfig.generatorLevel * 2.0));
    bool isNoise = (m_devices->captureConfig.generatorType == "WhiteNoise");
    m_genFreqLabel->setEnabled(!isNoise);
    m_genFreqSpin->setEnabled(!isNoise);
    m_genFreqSlider->setEnabled(!isNoise);

    // 3. Refresh Playback Devices List & CoreAudio controls
    populateDeviceCombo(m_pbDeviceCombo, m_pbWarningWidget, m_devices->playbackDevices,
                        m_devices->playbackConfig.deviceName());

    int pbBackendIdx = m_pbBackendCombo->findData(static_cast<int>(m_devices->playbackConfig.backend));
    if (pbBackendIdx >= 0) {
        m_pbBackendCombo->setCurrentIndex(pbBackendIdx);
        int stackIdx = 0;
        switch (m_devices->playbackConfig.backend) {
        case AudioBackendType::CoreAudio:
        case AudioBackendType::WASAPI:
        case AudioBackendType::ASIO:
        case AudioBackendType::ALSA:
        case AudioBackendType::PulseAudio:
            stackIdx = 0;
            break;
        case AudioBackendType::RawFile:
        case AudioBackendType::WavFile:
        case AudioBackendType::SignalGenerator:
            stackIdx = 1;
            break;
        }
        m_pbStack->setCurrentIndex(stackIdx);
    }

    // Playback Channels (Device Channels combo vs spinbox)
    auto pbSuppCh = m_devices->playbackConfig.supportedChannels();
    if (!pbSuppCh.empty()) {
        m_pbDevChannelsCombo->show();
        m_pbDevChannelsSpin->hide();
        m_pbDevChannelsCombo->blockSignals(true);
        m_pbDevChannelsCombo->clear();
        for (int ch : pbSuppCh) {
            m_pbDevChannelsCombo->addItem(QString::number(ch), ch);
        }
        int chIdx = m_pbDevChannelsCombo->findData(m_devices->playbackConfig.deviceChannels);
        if (chIdx >= 0)
            m_pbDevChannelsCombo->setCurrentIndex(chIdx);
        m_pbDevChannelsCombo->blockSignals(false);
    } else {
        m_pbDevChannelsCombo->hide();
        m_pbDevChannelsSpin->show();
        m_pbDevChannelsSpin->setValue(m_devices->playbackConfig.deviceChannels);
    }

    int pbDevCh = m_devices->playbackConfig.deviceChannels;
    m_pbStreamChannelsSpin->setRange(1, std::max(1, pbDevCh));
    m_pbStreamChannelsSpin->setValue(m_devices->playbackConfig.channels);

    // Playback Sample Rate
    m_pbRateCombo->blockSignals(true);
    m_pbRateCombo->clear();
    auto pbRates = m_devices->playbackRateOptions();
    for (int r : pbRates) {
        m_pbRateCombo->addItem(formatSampleRate(r), r);
    }
    int pbRateIdx = m_pbRateCombo->findData(m_devices->playbackConfig.sampleRate);
    if (pbRateIdx >= 0)
        m_pbRateCombo->setCurrentIndex(pbRateIdx);
    m_pbRateCombo->blockSignals(false);

    // Playback Sample Format
    auto pbFormats = m_devices->playbackConfig.supportedFormats();
    if (!pbFormats.empty()) {
        m_pbFormatCombo->show();
        m_pbFormatLabel->hide();
        m_pbFormatCombo->blockSignals(true);
        m_pbFormatCombo->clear();
        for (const auto& fmt : pbFormats) {
            m_pbFormatCombo->addItem(QString::fromStdString(fmt));
        }
        m_pbFormatCombo->setCurrentText(QString::fromStdString(m_devices->playbackConfig.format));
        m_pbFormatCombo->blockSignals(false);
    } else {
        m_pbFormatCombo->hide();
        m_pbFormatLabel->show();
        m_pbFormatLabel->setText(QString::fromStdString(m_devices->playbackConfig.format));
    }

    m_exclusiveModeCheck->setChecked(m_devices->exclusiveMode);
    m_outputDoPCheck->setChecked(m_devices->playbackConfig.outputDoP);

    int filterIdx = m_sdmFilterCombo->findData(static_cast<int>(m_devices->playbackConfig.dopEncoderFilter));
    if (filterIdx >= 0)
        m_sdmFilterCombo->setCurrentIndex(filterIdx);

    updateDoPCapability();

    // 4. Refresh Playback File View
    m_pbRawFilePathEdit->setText(QString::fromStdString(m_devices->playbackConfig.filename));
    m_pbRawFileFormatCombo->setCurrentText(QString::fromStdString(m_devices->playbackConfig.fileFormat));
    m_pbRawFileChannelsSpin->setValue(m_devices->playbackConfig.channels);

    // 5. Refresh Processing Settings
    int chunkIdx = m_chunkSizeCombo->findData(m_settings->chunkSize);
    if (chunkIdx >= 0)
        m_chunkSizeCombo->setCurrentIndex(chunkIdx);
    updateLatencyText();

    m_enableRateAdjustCheck->setChecked(m_settings->enableRateAdjust);
    m_queueLimitSpin->setValue(m_settings->queuelimit);
    m_stopOnRateChangeCheck->setChecked(m_settings->stopOnRateChange);

    m_measureIntervalSpin->setValue(m_settings->rateMeasureInterval);
    m_measureIntervalSlider->setValue(static_cast<int>(m_settings->rateMeasureInterval * 10.0));
    m_measureIntervalValLabel->setText(QString("%1 s").arg(m_settings->rateMeasureInterval, 0, 'f', 1));

    m_multithreadedCheck->setChecked(m_settings->multithreaded);
    m_workerThreadsRow->setVisible(m_settings->multithreaded);
    m_workerThreadsSpin->setValue(m_settings->workerThreads);

    m_isRefreshing = false;
}

void DevicePickerView::updateLatencyText() {
    if (!m_latencyLabel || !m_devices)
        return;
    double ms = m_devices->latencyMs();
    m_latencyLabel->setText(QString("(%1 ms latency)").arg(ms, 0, 'f', 1));
}

void DevicePickerView::applySettings() {
    if (m_isRefreshing)
        return;

    // 1. Processing settings
    if (m_chunkSizeCombo)
        m_settings->chunkSize = m_chunkSizeCombo->currentData().toInt();
    if (m_enableRateAdjustCheck)
        m_settings->enableRateAdjust = m_enableRateAdjustCheck->isChecked();
    if (m_queueLimitSpin)
        m_settings->queuelimit = m_queueLimitSpin->value();
    if (m_stopOnRateChangeCheck)
        m_settings->stopOnRateChange = m_stopOnRateChangeCheck->isChecked();
    if (m_measureIntervalSpin)
        m_settings->rateMeasureInterval = m_measureIntervalSpin->value();
    if (m_multithreadedCheck)
        m_settings->multithreaded = m_multithreadedCheck->isChecked();
    if (m_workerThreadsSpin)
        m_settings->workerThreads = m_workerThreadsSpin->value();
    m_settings->savePreferences();

    // 2. Capture settings
    DeviceConfig capCfg = m_devices->captureConfig;
    if (m_capBackendCombo->currentIndex() >= 0) {
        capCfg.backend = static_cast<AudioBackendType>(m_capBackendCombo->currentData().toInt());
    }

    if (capCfg.backend == AudioBackendType::CoreAudio) {
        if (m_capDeviceCombo->currentIndex() >= 0) {
            std::string selectedDev = m_capDeviceCombo->currentData().toString().toStdString();
            capCfg.setDeviceName(selectedDev);
        }

        auto capSuppCh = capCfg.supportedChannels();
        if (!capSuppCh.empty() && m_capDevChannelsCombo->currentIndex() >= 0) {
            capCfg.deviceChannels = m_capDevChannelsCombo->currentData().toInt();
        } else {
            capCfg.deviceChannels = m_capDevChannelsSpin->value();
        }
        capCfg.channels = m_capStreamChannelsSpin->value();

        if (m_settings->resamplerEnabled && m_capRateCombo->isVisible() && m_capRateCombo->currentIndex() >= 0) {
            capCfg.sampleRate = m_capRateCombo->currentData().toInt();
        }

        auto capFormats = capCfg.supportedFormats();
        if (!capFormats.empty() && m_capFormatCombo->isVisible()) {
            capCfg.format = m_capFormatCombo->currentText().toStdString();
        }

        capCfg.bypassDoP = m_bypassDoPCheck->isChecked();
        if (m_dopCutoffCombo->currentIndex() >= 0) {
            capCfg.dopCutoffHz = m_dopCutoffCombo->currentData().toDouble();
        }
    } else if (capCfg.backend == AudioBackendType::RawFile) {
        capCfg.filename = m_capRawFilePathEdit->text().toStdString();
        capCfg.fileFormat = m_capRawFileFormatCombo->currentText().toStdString();
        capCfg.channels = m_capRawFileChannelsSpin->value();
        capCfg.deviceChannels = capCfg.channels;
        capCfg.skipBytes = m_capRawSkipBytesSpin->value();
        capCfg.readBytes = m_capRawReadBytesSpin->value();
        capCfg.extraSamples = m_capRawExtraSamplesSpin->value();
        capCfg.isWav = false;
    } else if (capCfg.backend == AudioBackendType::WavFile) {
        capCfg.filename = m_capWavFilePathEdit->text().toStdString();
        capCfg.skipBytes = m_capWavSkipBytesSpin->value();
        capCfg.readBytes = m_capWavReadBytesSpin->value();
        capCfg.extraSamples = m_capWavExtraSamplesSpin->value();
        capCfg.isWav = true;
    } else if (capCfg.backend == AudioBackendType::SignalGenerator) {
        capCfg.generatorType = m_genTypeCombo->currentText().toStdString();
        capCfg.channels = m_genChannelsSpin->value();
        capCfg.deviceChannels = capCfg.channels;
        capCfg.generatorFreq = m_genFreqSpin->value();
        capCfg.generatorLevel = m_genLevelSpin->value();
    }
    m_devices->setCaptureConfig(capCfg);

    // 3. Playback settings
    DeviceConfig pbCfg = m_devices->playbackConfig;
    if (m_pbBackendCombo->currentIndex() >= 0) {
        pbCfg.backend = static_cast<AudioBackendType>(m_pbBackendCombo->currentData().toInt());
    }

    if (pbCfg.backend == AudioBackendType::CoreAudio) {
        if (m_pbDeviceCombo->currentIndex() >= 0) {
            std::string selectedDev = m_pbDeviceCombo->currentData().toString().toStdString();
            pbCfg.setDeviceName(selectedDev);
        }

        auto pbSuppCh = pbCfg.supportedChannels();
        if (!pbSuppCh.empty() && m_pbDevChannelsCombo->currentIndex() >= 0) {
            pbCfg.deviceChannels = m_pbDevChannelsCombo->currentData().toInt();
        } else {
            pbCfg.deviceChannels = m_pbDevChannelsSpin->value();
        }
        pbCfg.channels = m_pbStreamChannelsSpin->value();

        if (m_pbRateCombo->currentIndex() >= 0) {
            pbCfg.sampleRate = m_pbRateCombo->currentData().toInt();
        }

        auto pbFormats = pbCfg.supportedFormats();
        if (!pbFormats.empty() && m_pbFormatCombo->isVisible()) {
            pbCfg.format = m_pbFormatCombo->currentText().toStdString();
        }

        pbCfg.outputDoP = m_outputDoPCheck->isChecked();
        if (m_sdmFilterCombo->currentIndex() >= 0) {
            pbCfg.dopEncoderFilter = static_cast<SDMFilter>(m_sdmFilterCombo->currentData().toInt());
        }
    } else if (pbCfg.backend == AudioBackendType::RawFile) {
        pbCfg.filename = m_pbRawFilePathEdit->text().toStdString();
        pbCfg.fileFormat = m_pbRawFileFormatCombo->currentText().toStdString();
        pbCfg.channels = m_pbRawFileChannelsSpin->value();
        pbCfg.deviceChannels = pbCfg.channels;
    }
    m_devices->setPlaybackConfig(pbCfg);

    m_devices->setExclusiveMode(m_exclusiveModeCheck->isChecked());
    m_devices->refreshDeviceCapabilities();
    if (m_devices->onConfigChanged) {
        m_devices->onConfigChanged();
    }
}
