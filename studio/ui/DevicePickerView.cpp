#include "ui/DevicePickerView.h"

#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

DevicePickerView::DevicePickerView(std::shared_ptr<AudioDeviceManager> devices, std::shared_ptr<AudioSettings> settings,
                                   QWidget* parent)
    : QWidget(parent), m_devices(devices), m_settings(settings) {
    setupUi();

    connect(m_devices.get(), &AudioDeviceManager::devicesRefreshed, this, &DevicePickerView::refreshUi,
            Qt::QueuedConnection);
    connect(m_devices.get(), &AudioDeviceManager::configChanged, this, &DevicePickerView::refreshUi,
            Qt::QueuedConnection);
    if (m_settings) {
        connect(m_settings.get(), &AudioSettings::settingsChanged, this, &DevicePickerView::refreshUi,
                Qt::QueuedConnection);
    }

    refreshUi();
}

static void synchronizeFormLabels(const QList<QFormLayout*>& forms) {
    int maxW = 0;
    QList<QLabel*> labels;
    for (auto* form : forms) {
        if (!form)
            continue;
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        for (int i = 0; i < form->rowCount(); ++i) {
            auto* item = form->itemAt(i, QFormLayout::LabelRole);
            if (item && item->widget()) {
                if (auto* lbl = qobject_cast<QLabel*>(item->widget())) {
                    labels.append(lbl);
                    lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                    int w = lbl->fontMetrics().horizontalAdvance(lbl->text());
                    if (w > maxW)
                        maxW = w;
                }
            }
        }
    }
    for (auto* lbl : labels) {
        lbl->setFixedWidth(maxW);
    }
}

void DevicePickerView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);

    // 1. Capture Group
    auto capGroup = new QGroupBox(tr("Capture (Input)"), container);
    auto capLayout = new QVBoxLayout(capGroup);

    m_capBackendForm = new QFormLayout();
    m_capBackendForm->setContentsMargins(0, 0, 0, 0);
    m_capBackendCombo = new QComboBox(capGroup);
#if defined(ENABLE_COREAUDIO)
    m_capBackendCombo->addItem("CoreAudio", static_cast<int>(AudioBackendType::CoreAudio));
#endif
#if defined(ENABLE_WASAPI)
    m_capBackendCombo->addItem("WASAPI", static_cast<int>(AudioBackendType::WASAPI));
#endif
#if defined(ENABLE_ASIO)
    m_capBackendCombo->addItem("ASIO", static_cast<int>(AudioBackendType::ASIO));
#endif
#if defined(ENABLE_ALSA)
    m_capBackendCombo->addItem("ALSA", static_cast<int>(AudioBackendType::ALSA));
#endif
#if defined(ENABLE_PIPEWIRE)
    m_capBackendCombo->addItem("PipeWire", static_cast<int>(AudioBackendType::PipeWire));
#endif
    m_capBackendCombo->addItem("RawFile", static_cast<int>(AudioBackendType::RawFile));
    m_capBackendCombo->addItem("WavFile", static_cast<int>(AudioBackendType::WavFile));
    m_capBackendCombo->addItem("SignalGenerator", static_cast<int>(AudioBackendType::SignalGenerator));

    auto getCapStackIndex = [](AudioBackendType backend) {
        switch (backend) {
#if defined(ENABLE_COREAUDIO)
        case AudioBackendType::CoreAudio:
#endif
#if defined(ENABLE_WASAPI)
        case AudioBackendType::WASAPI:
#endif
#if defined(ENABLE_ASIO)
        case AudioBackendType::ASIO:
#endif
#if defined(ENABLE_ALSA)
        case AudioBackendType::ALSA:
#endif
#if defined(ENABLE_PIPEWIRE)
        case AudioBackendType::PipeWire:
#endif
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
        QTimer::singleShot(0, [this]() { applySettings(); });
    });

    auto capBackendRow = new QWidget(capGroup);
    auto capBackendBox = new QHBoxLayout(capBackendRow);
    capBackendBox->setContentsMargins(0, 0, 0, 0);
    capBackendBox->addWidget(m_capBackendCombo);
    capBackendBox->addStretch();
    m_capBackendForm->addRow(tr("Backend:"), capBackendRow);
    capLayout->addLayout(m_capBackendForm);

    m_capStack = new QStackedWidget(capGroup);
    m_capStack->addWidget(createCapCoreAudioView());
    m_capStack->addWidget(createCapFileView(false));
    m_capStack->addWidget(createCapFileView(true));
    m_capStack->addWidget(createCapGeneratorView());
    capLayout->addWidget(m_capStack);

    mainLayout->addWidget(capGroup);

    // 2. Playback Group
    auto pbGroup = new QGroupBox(tr("Playback (Output)"), container);
    auto pbLayout = new QVBoxLayout(pbGroup);

    m_pbBackendForm = new QFormLayout();
    m_pbBackendForm->setContentsMargins(0, 0, 0, 0);
    m_pbBackendCombo = new QComboBox(pbGroup);
#if defined(ENABLE_COREAUDIO)
    m_pbBackendCombo->addItem("CoreAudio", static_cast<int>(AudioBackendType::CoreAudio));
#endif
#if defined(ENABLE_WASAPI)
    m_pbBackendCombo->addItem("WASAPI", static_cast<int>(AudioBackendType::WASAPI));
#endif
#if defined(ENABLE_ASIO)
    m_pbBackendCombo->addItem("ASIO", static_cast<int>(AudioBackendType::ASIO));
#endif
#if defined(ENABLE_ALSA)
    m_pbBackendCombo->addItem("ALSA", static_cast<int>(AudioBackendType::ALSA));
#endif
#if defined(ENABLE_PIPEWIRE)
    m_pbBackendCombo->addItem("PipeWire", static_cast<int>(AudioBackendType::PipeWire));
#endif
    m_pbBackendCombo->addItem("RawFile", static_cast<int>(AudioBackendType::RawFile));
    m_pbBackendCombo->addItem("WavFile", static_cast<int>(AudioBackendType::WavFile));

    auto getPbStackIndex = [](AudioBackendType backend) {
        switch (backend) {
#if defined(ENABLE_COREAUDIO)
        case AudioBackendType::CoreAudio:
#endif
#if defined(ENABLE_WASAPI)
        case AudioBackendType::WASAPI:
#endif
#if defined(ENABLE_ASIO)
        case AudioBackendType::ASIO:
#endif
#if defined(ENABLE_ALSA)
        case AudioBackendType::ALSA:
#endif
#if defined(ENABLE_PIPEWIRE)
        case AudioBackendType::PipeWire:
#endif
            return 0;
        case AudioBackendType::RawFile:
            return 1;
        case AudioBackendType::WavFile:
            return 2;
        case AudioBackendType::SignalGenerator:
            return 0;
        }
        return 0;
    };

    connect(m_pbBackendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, getPbStackIndex](int) {
        if (m_isRefreshing)
            return;
        AudioBackendType b = static_cast<AudioBackendType>(m_pbBackendCombo->currentData().toInt());
        m_pbStack->setCurrentIndex(getPbStackIndex(b));
        QTimer::singleShot(0, [this]() { applySettings(); });
    });

    auto pbBackendRow = new QWidget(pbGroup);
    auto pbBackendBox = new QHBoxLayout(pbBackendRow);
    pbBackendBox->setContentsMargins(0, 0, 0, 0);
    pbBackendBox->addWidget(m_pbBackendCombo);
    pbBackendBox->addStretch();
    m_pbBackendForm->addRow(tr("Backend:"), pbBackendRow);
    pbLayout->addLayout(m_pbBackendForm);

    m_pbStack = new QStackedWidget(pbGroup);
    m_pbStack->addWidget(createPbCoreAudioView());
    m_pbStack->addWidget(createPbFileView(false));
    m_pbStack->addWidget(createPbFileView(true));
    pbLayout->addWidget(m_pbStack);

    mainLayout->addWidget(pbGroup);

    // 3. Processing Group
    auto procGroup = new QGroupBox(tr("Processing"), container);
    m_procForm = new QFormLayout(procGroup);

    auto chunkLayout = new QHBoxLayout();
    m_chunkSizeCombo = new QComboBox(procGroup);
    for (int size : {256, 512, 1024, 2048, 4096, 8192, 16384, 32768}) {
        m_chunkSizeCombo->addItem(QString("%1 samples").arg(size), size);
    }
    chunkLayout->addWidget(m_chunkSizeCombo);

    m_latencyLabel = new QLabel(procGroup);
    connect(m_chunkSizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() {
            if (m_chunkSizeCombo && m_settings) {
                m_settings->chunkSize = m_chunkSizeCombo->currentData().toInt();
            }
            updateLatencyText();
            applySettings();
        });
    });
    chunkLayout->addWidget(m_latencyLabel);
    chunkLayout->addStretch();
    m_procForm->addRow(tr("Chunk Size:"), chunkLayout);

    m_enableRateAdjustCheck = new QCheckBox(tr("Enable Rate Adjust"), procGroup);
    connect(m_enableRateAdjustCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    m_procForm->addRow(m_enableRateAdjustCheck);

    m_rateAdjustSub = new QLabel(tr("Compensate for clock drift between capture and playback devices"), procGroup);
    QFont subFont = m_rateAdjustSub->font();
    subFont.setPointSize(subFont.pointSize() > 2 ? subFont.pointSize() - 1 : 10);
    m_rateAdjustSub->setFont(subFont);
    m_rateAdjustSub->setWordWrap(true);
    m_procForm->addRow(m_rateAdjustSub);

    m_queueLimitSpin = new QSpinBox(procGroup);
    m_queueLimitSpin->setRange(1, 32);
    connect(m_queueLimitSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    m_procForm->addRow(tr("Queue Limit:"), m_queueLimitSpin);

    m_stopOnRateChangeCheck = new QCheckBox(tr("Stop on Rate Change"), procGroup);
    connect(m_stopOnRateChangeCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    m_procForm->addRow(m_stopOnRateChangeCheck);

    auto intervalBox = new QHBoxLayout();
    m_measureIntervalSlider = new QSlider(Qt::Horizontal, procGroup);
    m_measureIntervalSlider->setRange(1, 100); // 0.1 to 10.0 s

    m_measureIntervalValLabel = new QLabel(procGroup);
    m_measureIntervalValLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_measureIntervalValLabel->setMinimumWidth(50);

    connect(m_measureIntervalSlider, &QSlider::valueChanged, [this](int val) {
        if (m_isRefreshing)
            return;
        double dVal = val / 10.0;
        m_measureIntervalValLabel->setText(QString("%1 s").arg(dVal, 0, 'f', 1));
        applySettings();
    });
    intervalBox->addWidget(m_measureIntervalSlider);
    intervalBox->addWidget(m_measureIntervalValLabel);
    intervalBox->addStretch();
    m_procForm->addRow(tr("Measure Interval:"), intervalBox);

    m_multithreadedCheck = new QCheckBox(tr("Multithreaded"), procGroup);
    connect(m_multithreadedCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_isRefreshing)
            return;
        if (m_procForm && m_workerThreadsSpin) {
            m_procForm->setRowVisible(m_workerThreadsSpin, checked);
        }
        applySettings();
    });
    m_procForm->addRow(m_multithreadedCheck);

    m_workerThreadsSpin = new QSpinBox(procGroup);
    m_workerThreadsSpin->setRange(0, 32);
    m_workerThreadsSpin->setSpecialValueText(tr("Auto"));
    connect(m_workerThreadsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    m_procForm->addRow(tr("Worker Threads:"), m_workerThreadsSpin);

    mainLayout->addWidget(procGroup);

    scroll->setWidget(container);

    synchronizeFormLabels({m_capBackendForm, m_capCoreAudioForm, m_capRawFileForm, m_capWavFileForm, m_capGenForm,
                           m_pbBackendForm, m_pbCoreAudioForm, m_pbRawFileForm, m_pbWavFileForm, m_procForm});

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

void DevicePickerView::populateDeviceList(QListWidget* listWidget, QWidget* warningWidget,
                                          const std::vector<AudioDevice>& devices,
                                          const std::optional<std::string>& selectedDeviceName) {
    if (!listWidget || !warningWidget)
        return;

    listWidget->blockSignals(true);
    listWidget->clear();

    if (devices.empty()) {
        warningWidget->show();
        listWidget->hide();
        listWidget->blockSignals(false);
        return;
    }

    warningWidget->hide();
    listWidget->show();

    auto defaultItem = new QListWidgetItem(tr("System Default"), listWidget);
    defaultItem->setData(Qt::UserRole, QString());

    bool defaultSelected = !selectedDeviceName.has_value() || selectedDeviceName.value().empty();
    int selectRow = defaultSelected ? 0 : -1;

    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& dev = devices[i];
        std::string devDisplay;
        if (dev.id.empty() || dev.id == dev.name) {
            devDisplay = dev.name;
        } else {
            devDisplay = dev.id + " (" + dev.name + ")";
        }
        auto item = new QListWidgetItem(QString::fromStdString(devDisplay), listWidget);
        item->setData(Qt::UserRole, QString::fromStdString(dev.id));

        if (selectRow < 0 && selectedDeviceName.has_value() && !selectedDeviceName.value().empty() &&
            (selectedDeviceName.value() == dev.id || selectedDeviceName.value() == dev.name)) {
            selectRow = static_cast<int>(i + 1);
        }
    }

    if (selectRow < 0) {
        selectRow = 0;
    }
    listWidget->setCurrentRow(selectRow);
    listWidget->blockSignals(false);
}

QWidget* DevicePickerView::createCapCoreAudioView() {
    auto w = new QWidget();
    m_capCoreAudioForm = new QFormLayout(w);
    m_capCoreAudioForm->setContentsMargins(0, 0, 0, 0);

    m_capWarningLabel = new QLabel(tr("⚠️  No devices found"), w);

    m_capDeviceList = new QListWidget(w);
    m_capDeviceList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_capDeviceList->setMaximumHeight(130);

    connect(m_capDeviceList, &QListWidget::currentItemChanged, [this](QListWidgetItem* current) {
        if (m_isRefreshing || !current)
            return;
        std::string devId = current->data(Qt::UserRole).toString().toStdString();
        m_devices->captureConfig.setDeviceName(devId);
        m_devices->refreshDeviceCapabilities();
        m_devices->validateSampleRates();
        applySettings();
        refreshUi();
    });

    m_capDeviceContainer = new QWidget(w);
    auto devBox = new QVBoxLayout(m_capDeviceContainer);
    devBox->setContentsMargins(0, 0, 0, 0);
    devBox->addWidget(m_capWarningLabel);
    devBox->addWidget(m_capDeviceList);
    m_capCoreAudioForm->addRow(tr("Device:"), m_capDeviceContainer);

    m_capDevChannelsCombo = new QComboBox(w);
    m_capDevChannelsSpin = new QSpinBox(w);
    m_capDevChannelsSpin->setRange(1, 32);

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
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this, onDevChChanged]() {
            if (m_capDevChannelsCombo->currentIndex() >= 0) {
                onDevChChanged(m_capDevChannelsCombo->currentData().toInt());
            }
        });
    });

    m_capDevChannelsRow = new QWidget(w);
    auto devChLayout = new QHBoxLayout(m_capDevChannelsRow);
    devChLayout->setContentsMargins(0, 0, 0, 0);
    devChLayout->addWidget(m_capDevChannelsCombo);
    devChLayout->addWidget(m_capDevChannelsSpin);
    devChLayout->addStretch();
    m_capCoreAudioForm->addRow(tr("Device Channels:"), m_capDevChannelsRow);

    m_capStreamChannelsSpin = new QSpinBox(w);
    m_capStreamChannelsSpin->setRange(1, 32);
    connect(m_capStreamChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    m_capCoreAudioForm->addRow(tr("Stream Channels:"), m_capStreamChannelsSpin);

    m_capRateRow = new QWidget(w);
    auto rateBox = new QHBoxLayout(m_capRateRow);
    rateBox->setContentsMargins(0, 0, 0, 0);

    m_capRateCombo = new QComboBox(m_capRateRow);
    connect(m_capRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() { applySettings(); });
    });

    m_capRateLabel = new QLabel(m_capRateRow);
    m_capRateLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    rateBox->addWidget(m_capRateCombo);
    rateBox->addWidget(m_capRateLabel);
    rateBox->addStretch();
    m_capCoreAudioForm->addRow(tr("Sample Rate:"), m_capRateRow);

    m_capFormatRow = new QWidget(w);
    auto fmtBox = new QHBoxLayout(m_capFormatRow);
    fmtBox->setContentsMargins(0, 0, 0, 0);

    m_capFormatCombo = new QComboBox(m_capFormatRow);
    connect(m_capFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() { applySettings(); });
    });

    m_capFormatLabel = new QLabel(m_capFormatRow);
    m_capFormatLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    fmtBox->addWidget(m_capFormatCombo);
    fmtBox->addWidget(m_capFormatLabel);
    fmtBox->addStretch();
    m_capCoreAudioForm->addRow(tr("Format:"), m_capFormatRow);

    m_bypassDoPCheck = new QCheckBox(tr("Bypass DoP Detection"), w);
    connect(m_bypassDoPCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_isRefreshing)
            return;
        m_dopCutoffCombo->setEnabled(!checked);
        applySettings();
    });
    m_capCoreAudioForm->addRow(m_bypassDoPCheck);

    m_dopCutoffCombo = new QComboBox(w);
    m_dopCutoffCombo->addItem("20 kHz", 20000.0);
    m_dopCutoffCombo->addItem("25 kHz", 25000.0);
    m_dopCutoffCombo->addItem("30 kHz", 30000.0);
    m_dopCutoffCombo->addItem("40 kHz", 40000.0);
    m_dopCutoffCombo->addItem("50 kHz", 50000.0);
    connect(m_dopCutoffCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() { applySettings(); });
    });
    m_capCoreAudioForm->addRow(tr("DoP Cutoff:"), m_dopCutoffCombo);

    m_dopCutoffHint = new QLabel(tr("Lower cutoff = higher SINAD; higher cutoff preserves more ultrasonic content"), w);
    m_dopCutoffHint->setWordWrap(true);
    QFont hintFont = m_dopCutoffHint->font();
    hintFont.setPointSize(hintFont.pointSize() > 2 ? hintFont.pointSize() - 1 : 10);
    m_dopCutoffHint->setFont(hintFont);
    m_capCoreAudioForm->addRow(m_dopCutoffHint);

    m_capWasapiExclusiveCheck = new QCheckBox(tr("WASAPI Exclusive Mode"), w);
    connect(m_capWasapiExclusiveCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(m_capWasapiExclusiveCheck);

    m_capWasapiLoopbackCheck = new QCheckBox(tr("WASAPI Loopback (Capture from Playback Device)"), w);
    connect(m_capWasapiLoopbackCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing) {
            applySettings();
            refreshUi();
        }
    });
    m_capCoreAudioForm->addRow(m_capWasapiLoopbackCheck);

    m_capWasapiPollingCheck = new QCheckBox(tr("WASAPI Polling Mode"), w);
    connect(m_capWasapiPollingCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(m_capWasapiPollingCheck);

    m_capAlsaStopInactiveCheck = new QCheckBox(tr("Stop Streams When Inactive"), w);
    connect(m_capAlsaStopInactiveCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(m_capAlsaStopInactiveCheck);

    m_capAlsaThreadedCheck = new QCheckBox(tr("Threaded Ring Buffer Mode"), w);
    connect(m_capAlsaThreadedCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(m_capAlsaThreadedCheck);

    m_capAlsaLinkVolumeEdit = new QLineEdit(w);
    m_capAlsaLinkVolumeEdit->setPlaceholderText("e.g. Master");
    connect(m_capAlsaLinkVolumeEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(tr("Link Volume:"), m_capAlsaLinkVolumeEdit);

    m_capAlsaLinkMuteEdit = new QLineEdit(w);
    m_capAlsaLinkMuteEdit->setPlaceholderText("e.g. Master");
    connect(m_capAlsaLinkMuteEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(tr("Link Mute:"), m_capAlsaLinkMuteEdit);

    m_capPwNodeNameEdit = new QLineEdit(w);
    m_capPwNodeNameEdit->setPlaceholderText("e.g. cdsp-capture");
    connect(m_capPwNodeNameEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(tr("Node Name:"), m_capPwNodeNameEdit);

    m_capPwNodeDescEdit = new QLineEdit(w);
    m_capPwNodeDescEdit->setPlaceholderText("e.g. CDSP Capture");
    connect(m_capPwNodeDescEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(tr("Node Description:"), m_capPwNodeDescEdit);

    m_capPwNodeGroupEdit = new QLineEdit(w);
    m_capPwNodeGroupEdit->setPlaceholderText("e.g. cdsp");
    connect(m_capPwNodeGroupEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(tr("Node Group:"), m_capPwNodeGroupEdit);

    m_capPwAutoconnectEdit = new QLineEdit(w);
    connect(m_capPwAutoconnectEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_capCoreAudioForm->addRow(tr("Autoconnect To:"), m_capPwAutoconnectEdit);

    return w;
}

QWidget* DevicePickerView::createCapFileView(bool isWav) {
    auto w = new QWidget();
    auto form = new QFormLayout(w);
    form->setContentsMargins(0, 0, 0, 0);

    auto fileBox = new QHBoxLayout();

    if (isWav) {
        m_capWavFileForm = form;
        m_capWavFilePathEdit = new QLineEdit(w);
        m_capWavFilePathEdit->setPlaceholderText("e.g. /path/to/audio.wav");
        m_capWavFilePathEdit->setClearButtonEnabled(true);
        connect(m_capWavFilePathEdit, &QLineEdit::editingFinished, [this]() {
            if (!m_isRefreshing)
                applySettings();
        });
        fileBox->addWidget(m_capWavFilePathEdit);

        auto browseBtn = new QPushButton(tr("Browse..."), w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path =
                QFileDialog::getOpenFileName(w, tr("Select WAV File"), "", tr("WAV Files (*.wav);;All Files (*)"));
            if (!path.isEmpty()) {
                m_capWavFilePathEdit->setText(path);
                applySettings();
            }
        });
        fileBox->addWidget(browseBtn);
        form->addRow(tr("File Path:"), fileBox);

        auto noteLbl = new QLabel(tr("Sample rate, format, and channel count are parsed from the file header"), w);
        noteLbl->setWordWrap(true);
        QFont noteFont = noteLbl->font();
        noteFont.setPointSize(noteFont.pointSize() > 2 ? noteFont.pointSize() - 1 : 10);
        noteLbl->setFont(noteFont);
        form->addRow(noteLbl);

        m_capWavSkipBytesSpin = new QSpinBox(w);
        m_capWavSkipBytesSpin->setRange(0, 1000000);
        connect(m_capWavSkipBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Skip Bytes:"), m_capWavSkipBytesSpin);

        m_capWavReadBytesSpin = new QSpinBox(w);
        m_capWavReadBytesSpin->setRange(0, 100000000);
        m_capWavReadBytesSpin->setSpecialValueText(tr("0 (All)"));
        connect(m_capWavReadBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Read Bytes:"), m_capWavReadBytesSpin);

        m_capWavExtraSamplesSpin = new QSpinBox(w);
        m_capWavExtraSamplesSpin->setRange(0, 1000000);
        connect(m_capWavExtraSamplesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Extra Samples:"), m_capWavExtraSamplesSpin);

    } else {
        m_capRawFileForm = form;
        m_capRawFilePathEdit = new QLineEdit(w);
        m_capRawFilePathEdit->setPlaceholderText("e.g. /path/to/audio.raw");
        m_capRawFilePathEdit->setClearButtonEnabled(true);
        connect(m_capRawFilePathEdit, &QLineEdit::editingFinished, [this]() {
            if (!m_isRefreshing)
                applySettings();
        });
        fileBox->addWidget(m_capRawFilePathEdit);

        auto browseBtn = new QPushButton(tr("Browse..."), w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path = QFileDialog::getOpenFileName(w, tr("Select Raw File"), "",
                                                        tr("Raw Files (*.raw *.f64 *.f32);;All Files (*)"));
            if (!path.isEmpty()) {
                m_capRawFilePathEdit->setText(path);
                applySettings();
            }
        });
        fileBox->addWidget(browseBtn);
        form->addRow(tr("File Path:"), fileBox);

        m_capRawFileFormatCombo = new QComboBox(w);
        m_capRawFileFormatCombo->addItems(
            {"S16_LE", "S24_3_LE", "S24_4_LJ_LE", "S24_4_RJ_LE", "S32_LE", "F32_LE", "F64_LE"});
        connect(m_capRawFileFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Format:"), m_capRawFileFormatCombo);

        m_capRawFileChannelsSpin = new QSpinBox(w);
        m_capRawFileChannelsSpin->setRange(1, 32);
        connect(m_capRawFileChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Channels:"), m_capRawFileChannelsSpin);

        m_capRawSkipBytesSpin = new QSpinBox(w);
        m_capRawSkipBytesSpin->setRange(0, 1000000);
        connect(m_capRawSkipBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Skip Bytes:"), m_capRawSkipBytesSpin);

        m_capRawReadBytesSpin = new QSpinBox(w);
        m_capRawReadBytesSpin->setRange(0, 100000000);
        m_capRawReadBytesSpin->setSpecialValueText(tr("0 (All)"));
        connect(m_capRawReadBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Read Bytes:"), m_capRawReadBytesSpin);

        m_capRawExtraSamplesSpin = new QSpinBox(w);
        m_capRawExtraSamplesSpin->setRange(0, 1000000);
        connect(m_capRawExtraSamplesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Extra Samples:"), m_capRawExtraSamplesSpin);
    }

    return w;
}

QWidget* DevicePickerView::createCapGeneratorView() {
    auto w = new QWidget();
    m_capGenForm = new QFormLayout(w);
    m_capGenForm->setContentsMargins(0, 0, 0, 0);

    m_genTypeCombo = new QComboBox(w);
    m_genTypeCombo->addItems({"Sine", "Square", "WhiteNoise"});
    m_capGenForm->addRow(tr("Signal Type:"), m_genTypeCombo);

    m_genChannelsSpin = new QSpinBox(w);
    m_genChannelsSpin->setRange(1, 32);
    connect(m_genChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    m_capGenForm->addRow(tr("Channels:"), m_genChannelsSpin);

    auto freqBox = new QHBoxLayout();
    m_genFreqSpin = new QDoubleSpinBox(w);
    m_genFreqSpin->setRange(1.0, 20000.0);
    m_genFreqSpin->setSingleStep(1.0);
    m_genFreqSpin->setSuffix(" Hz");

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
    m_capGenForm->addRow(tr("Frequency:"), freqBox);

    auto levelBox = new QHBoxLayout();
    m_genLevelSpin = new QDoubleSpinBox(w);
    m_genLevelSpin->setRange(-100.0, 0.0);
    m_genLevelSpin->setSingleStep(0.5);
    m_genLevelSpin->setSuffix(" dB");

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
    m_capGenForm->addRow(tr("Level:"), levelBox);

    connect(m_genTypeCombo, &QComboBox::currentTextChanged, [this, freqBox](const QString& type) {
        bool isNoise = (type == "WhiteNoise");
        m_genFreqSpin->setEnabled(!isNoise);
        m_genFreqSlider->setEnabled(!isNoise);
        QWidget* freqLbl = m_capGenForm ? m_capGenForm->labelForField(freqBox) : nullptr;
        if (freqLbl)
            freqLbl->setEnabled(!isNoise);
        if (m_isRefreshing)
            return;
        applySettings();
    });

    return w;
}

QWidget* DevicePickerView::createPbCoreAudioView() {
    auto w = new QWidget();
    m_pbCoreAudioForm = new QFormLayout(w);
    m_pbCoreAudioForm->setContentsMargins(0, 0, 0, 0);

    m_pbWarningLabel = new QLabel(tr("⚠️  No devices found"), w);

    m_pbDeviceList = new QListWidget(w);
    m_pbDeviceList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pbDeviceList->setMaximumHeight(130);

    connect(m_pbDeviceList, &QListWidget::currentItemChanged, [this](QListWidgetItem* current) {
        if (m_isRefreshing || !current)
            return;
        std::string devId = current->data(Qt::UserRole).toString().toStdString();
        m_devices->playbackConfig.setDeviceName(devId);
        m_devices->refreshDeviceCapabilities();
        m_devices->validateSampleRates();
        applySettings();
        refreshUi();
    });

    m_pbDeviceContainer = new QWidget(w);
    auto devBox = new QVBoxLayout(m_pbDeviceContainer);
    devBox->setContentsMargins(0, 0, 0, 0);
    devBox->addWidget(m_pbWarningLabel);
    devBox->addWidget(m_pbDeviceList);
    m_pbCoreAudioForm->addRow(tr("Device:"), m_pbDeviceContainer);

    m_pbDevChannelsCombo = new QComboBox(w);
    m_pbDevChannelsSpin = new QSpinBox(w);
    m_pbDevChannelsSpin->setRange(1, 32);

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
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this, onDevChChanged]() {
            if (m_pbDevChannelsCombo->currentIndex() >= 0) {
                onDevChChanged(m_pbDevChannelsCombo->currentData().toInt());
            }
        });
    });

    m_pbDevChannelsRow = new QWidget(w);
    auto devChLayout = new QHBoxLayout(m_pbDevChannelsRow);
    devChLayout->setContentsMargins(0, 0, 0, 0);
    devChLayout->addWidget(m_pbDevChannelsCombo);
    devChLayout->addWidget(m_pbDevChannelsSpin);
    devChLayout->addStretch();
    m_pbCoreAudioForm->addRow(tr("Device Channels:"), m_pbDevChannelsRow);

    m_pbStreamChannelsSpin = new QSpinBox(w);
    m_pbStreamChannelsSpin->setRange(1, 32);
    connect(m_pbStreamChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    m_pbCoreAudioForm->addRow(tr("Stream Channels:"), m_pbStreamChannelsSpin);

    m_pbRateCombo = new QComboBox(w);
    connect(m_pbRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() {
            applySettings();
            updateDoPCapability();
        });
    });
    m_pbCoreAudioForm->addRow(tr("Sample Rate:"), m_pbRateCombo);

    m_pbFormatRow = new QWidget(w);
    auto fmtBox = new QHBoxLayout(m_pbFormatRow);
    fmtBox->setContentsMargins(0, 0, 0, 0);

    m_pbFormatCombo = new QComboBox(m_pbFormatRow);
    connect(m_pbFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() {
            applySettings();
            updateDoPCapability();
        });
    });

    m_pbFormatLabel = new QLabel(m_pbFormatRow);
    m_pbFormatLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    fmtBox->addWidget(m_pbFormatCombo);
    fmtBox->addWidget(m_pbFormatLabel);
    fmtBox->addStretch();
    m_pbCoreAudioForm->addRow(tr("Format:"), m_pbFormatRow);

    m_exclusiveModeCheck = new QCheckBox(tr("Exclusive Mode (Hog)"), w);
    connect(m_exclusiveModeCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    m_pbCoreAudioForm->addRow(m_exclusiveModeCheck);

    m_exclusiveModeHint =
        new QLabel(tr("Takes exclusive access to the output device, preventing other apps from using it"), w);
    m_exclusiveModeHint->setWordWrap(true);
    QFont hogHintFont = m_exclusiveModeHint->font();
    hogHintFont.setPointSize(hogHintFont.pointSize() > 2 ? hogHintFont.pointSize() - 1 : 10);
    m_exclusiveModeHint->setFont(hogHintFont);
    m_pbCoreAudioForm->addRow(m_exclusiveModeHint);

    m_pbWasapiPollingCheck = new QCheckBox(tr("WASAPI Polling Mode"), w);
    connect(m_pbWasapiPollingCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    m_pbCoreAudioForm->addRow(m_pbWasapiPollingCheck);

    m_pbAlsaThreadedCheck = new QCheckBox(tr("Threaded Ring Buffer Mode"), w);
    connect(m_pbAlsaThreadedCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    m_pbCoreAudioForm->addRow(m_pbAlsaThreadedCheck);

    m_pbPwNodeNameEdit = new QLineEdit(w);
    m_pbPwNodeNameEdit->setPlaceholderText("e.g. cdsp-playback");
    connect(m_pbPwNodeNameEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_pbCoreAudioForm->addRow(tr("Node Name:"), m_pbPwNodeNameEdit);

    m_pbPwNodeDescEdit = new QLineEdit(w);
    m_pbPwNodeDescEdit->setPlaceholderText("e.g. CDSP Playback");
    connect(m_pbPwNodeDescEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_pbCoreAudioForm->addRow(tr("Node Description:"), m_pbPwNodeDescEdit);

    m_pbPwNodeGroupEdit = new QLineEdit(w);
    m_pbPwNodeGroupEdit->setPlaceholderText("e.g. cdsp");
    connect(m_pbPwNodeGroupEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_pbCoreAudioForm->addRow(tr("Node Group:"), m_pbPwNodeGroupEdit);

    m_pbPwAutoconnectEdit = new QLineEdit(w);
    connect(m_pbPwAutoconnectEdit, &QLineEdit::editingFinished, [this]() {
        if (!m_isRefreshing)
            applySettings();
    });
    m_pbCoreAudioForm->addRow(tr("Autoconnect To:"), m_pbPwAutoconnectEdit);

    m_outputDoPCheck = new QCheckBox(tr("Output DoP (DSD-over-PCM)"), w);
    connect(m_outputDoPCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
        updateDoPCapability();
    });
    m_pbCoreAudioForm->addRow(m_outputDoPCheck);

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
        QTimer::singleShot(0, [this]() { applySettings(); });
    });
    m_pbCoreAudioForm->addRow(tr("SDM Filter:"), m_sdmFilterCombo);

    m_pbDopHintLabel = new QLabel(tr("Sample rate must be a DSD carrier rate to enable DoP output"), w);
    m_pbDopHintLabel->setWordWrap(true);
    QFont dopHintFont = m_pbDopHintLabel->font();
    dopHintFont.setPointSize(dopHintFont.pointSize() > 2 ? dopHintFont.pointSize() - 1 : 10);
    m_pbDopHintLabel->setFont(dopHintFont);
    m_pbCoreAudioForm->addRow(m_pbDopHintLabel);

    return w;
}

void DevicePickerView::updateDoPCapability() {
    if (!m_pbRateCombo || !m_outputDoPCheck || !m_sdmFilterCombo || !m_pbDopHintLabel)
        return;
    int currentRate = m_pbRateCombo->currentData().toInt();
    bool isCapable = (currentRate == 176400 || currentRate == 192000 || currentRate == 352800 ||
                      currentRate == 384000 || currentRate == 705600 || currentRate == 768000);

    m_outputDoPCheck->setEnabled(isCapable);

    QString formatStr;
    if (m_pbFormatCombo && m_pbFormatCombo->currentIndex() >= 0) {
        formatStr = m_pbFormatCombo->currentData().toString();
        if (formatStr.isEmpty())
            formatStr = m_pbFormatCombo->currentText();
    }
    if (formatStr.isEmpty() && m_devices && m_devices->playbackConfig.format.has_value()) {
        formatStr = QString::fromStdString(m_devices->playbackConfig.format.value());
    }
    bool isDsdFormat = formatStr.contains("DSD", Qt::CaseInsensitive);

    bool sdmEnabled = (isCapable && m_outputDoPCheck->isChecked()) || isDsdFormat;
    m_sdmFilterCombo->setEnabled(sdmEnabled);
    if (m_pbCoreAudioForm) {
        QWidget* filterLabel = m_pbCoreAudioForm->labelForField(m_sdmFilterCombo);
        if (filterLabel) {
            filterLabel->setEnabled(sdmEnabled);
        }
    }
    m_pbDopHintLabel->setVisible(!isCapable);
}

QWidget* DevicePickerView::createPbFileView(bool isWav) {
    auto w = new QWidget();
    auto form = new QFormLayout(w);
    form->setContentsMargins(0, 0, 0, 0);

    auto fileBox = new QHBoxLayout();

    if (isWav) {
        m_pbWavFileForm = form;
        m_pbWavFilePathEdit = new QLineEdit(w);
        m_pbWavFilePathEdit->setPlaceholderText("e.g. /path/to/audio.wav");
        m_pbWavFilePathEdit->setClearButtonEnabled(true);
        connect(m_pbWavFilePathEdit, &QLineEdit::editingFinished, [this]() {
            if (!m_isRefreshing)
                applySettings();
        });
        fileBox->addWidget(m_pbWavFilePathEdit);

        auto browseBtn = new QPushButton(tr("Browse..."), w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path =
                QFileDialog::getSaveFileName(w, tr("Select Output File"), "", tr("WAV Files (*.wav);;All Files (*)"));
            if (!path.isEmpty()) {
                m_pbWavFilePathEdit->setText(path);
                applySettings();
            }
        });
        fileBox->addWidget(browseBtn);
        form->addRow(tr("File Path:"), fileBox);

        m_pbWavFileFormatCombo = new QComboBox(w);
        m_pbWavFileFormatCombo->addItems({"S16_LE", "S24_3_LE", "S24_4_LJ_LE", "S32_LE", "F32_LE", "F64_LE"});
        connect(m_pbWavFileFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
            if (m_isRefreshing)
                return;
            QTimer::singleShot(0, [this]() { applySettings(); });
        });
        form->addRow(tr("Format:"), m_pbWavFileFormatCombo);

        m_pbWavFileChannelsSpin = new QSpinBox(w);
        m_pbWavFileChannelsSpin->setRange(1, 32);
        connect(m_pbWavFileChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Channels:"), m_pbWavFileChannelsSpin);

        m_pbWavUseRf64Combo = new QComboBox(w);
        m_pbWavUseRf64Combo->addItem(tr("Standard (RIFF)"), false);
        m_pbWavUseRf64Combo->addItem(tr("RF64 (64-bit)"), true);
        connect(m_pbWavUseRf64Combo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("WAV Format:"), m_pbWavUseRf64Combo);
    } else {
        m_pbRawFileForm = form;
        m_pbRawFilePathEdit = new QLineEdit(w);
        m_pbRawFilePathEdit->setPlaceholderText("e.g. /path/to/audio.raw");
        m_pbRawFilePathEdit->setClearButtonEnabled(true);
        connect(m_pbRawFilePathEdit, &QLineEdit::editingFinished, [this]() {
            if (!m_isRefreshing)
                applySettings();
        });
        fileBox->addWidget(m_pbRawFilePathEdit);

        auto browseBtn = new QPushButton(tr("Browse..."), w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path = QFileDialog::getSaveFileName(w, tr("Select Output File"), "",
                                                        tr("Raw Files (*.raw *.f64 *.f32);;All Files (*)"));
            if (!path.isEmpty()) {
                m_pbRawFilePathEdit->setText(path);
                applySettings();
            }
        });
        fileBox->addWidget(browseBtn);
        form->addRow(tr("File Path:"), fileBox);

        m_pbRawFileFormatCombo = new QComboBox(w);
        m_pbRawFileFormatCombo->addItems(
            {"S16_LE", "S24_3_LE", "S24_4_RJ_LE", "S24_4_LJ_LE", "S32_LE", "F32_LE", "F64_LE"});
        connect(m_pbRawFileFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
            if (m_isRefreshing)
                return;
            QTimer::singleShot(0, [this]() { applySettings(); });
        });
        form->addRow(tr("Format:"), m_pbRawFileFormatCombo);

        m_pbRawFileChannelsSpin = new QSpinBox(w);
        m_pbRawFileChannelsSpin->setRange(1, 32);
        connect(m_pbRawFileChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow(tr("Channels:"), m_pbRawFileChannelsSpin);
    }

    return w;
}

static int getCapStackIndex(AudioBackendType backend) {
    switch (backend) {
#if defined(ENABLE_COREAUDIO)
    case AudioBackendType::CoreAudio:
#endif
#if defined(ENABLE_WASAPI)
    case AudioBackendType::WASAPI:
#endif
#if defined(ENABLE_ASIO)
    case AudioBackendType::ASIO:
#endif
#if defined(ENABLE_ALSA)
    case AudioBackendType::ALSA:
#endif
#if defined(ENABLE_PIPEWIRE)
    case AudioBackendType::PipeWire:
#endif
        return 0;
    case AudioBackendType::RawFile:
        return 1;
    case AudioBackendType::WavFile:
        return 2;
    case AudioBackendType::SignalGenerator:
        return 3;
    }
    return 0;
}

static int getPbStackIndex(AudioBackendType backend) {
    switch (backend) {
#if defined(ENABLE_COREAUDIO)
    case AudioBackendType::CoreAudio:
#endif
#if defined(ENABLE_WASAPI)
    case AudioBackendType::WASAPI:
#endif
#if defined(ENABLE_ASIO)
    case AudioBackendType::ASIO:
#endif
#if defined(ENABLE_ALSA)
    case AudioBackendType::ALSA:
#endif
#if defined(ENABLE_PIPEWIRE)
    case AudioBackendType::PipeWire:
#endif
        return 0;
    case AudioBackendType::RawFile:
        return 1;
    case AudioBackendType::WavFile:
        return 2;
    case AudioBackendType::SignalGenerator:
        return 0;
    }
    return 0;
}

void DevicePickerView::refreshUi() {
    m_isRefreshing = true;

    bool isCapPw = false;
#if defined(ENABLE_PIPEWIRE)
    isCapPw = m_devices->captureConfig.backend == AudioBackendType::PipeWire;
#endif
    bool isCapWasapi = false;
#if defined(ENABLE_WASAPI)
    isCapWasapi = m_devices->captureConfig.backend == AudioBackendType::WASAPI;
#endif
    bool isCapAlsa = false;
#if defined(ENABLE_ALSA)
    isCapAlsa = m_devices->captureConfig.backend == AudioBackendType::ALSA;
#endif
    bool isPbPw = false;
#if defined(ENABLE_PIPEWIRE)
    isPbPw = m_devices->playbackConfig.backend == AudioBackendType::PipeWire;
#endif
    bool isPbWasapi = false;
#if defined(ENABLE_WASAPI)
    isPbWasapi = m_devices->playbackConfig.backend == AudioBackendType::WASAPI;
#endif
    bool isPbCoreAudio = false;
#if defined(ENABLE_COREAUDIO)
    isPbCoreAudio = m_devices->playbackConfig.backend == AudioBackendType::CoreAudio;
#endif
    bool isPbAlsa = false;
#if defined(ENABLE_ALSA)
    isPbAlsa = m_devices->playbackConfig.backend == AudioBackendType::ALSA;
#endif

    // 1. Refresh Capture Devices List & CoreAudio controls
    if (!isCapPw) {
        bool isLoopback = (isCapWasapi && m_devices->captureConfig.loopback);
        const auto& capDevs = isLoopback ? m_devices->playbackDevices : m_devices->captureDevices;
        populateDeviceList(m_capDeviceList, m_capWarningLabel, capDevs, m_devices->captureConfig.deviceName());
    } else {
        m_capWarningLabel->hide();
        m_capDeviceList->hide();
    }
    if (m_capCoreAudioForm && m_capDeviceContainer) {
        m_capCoreAudioForm->setRowVisible(m_capDeviceContainer, !isCapPw);
    }

    int capBackendIdx = m_capBackendCombo->findData(static_cast<int>(m_devices->captureConfig.backend));
    if (capBackendIdx >= 0) {
        m_capBackendCombo->blockSignals(true);
        m_capBackendCombo->setCurrentIndex(capBackendIdx);
        m_capBackendCombo->blockSignals(false);
    }
    m_capStack->setCurrentIndex(getCapStackIndex(m_devices->captureConfig.backend));

    // Capture Channels
    if (m_capCoreAudioForm && m_capDevChannelsRow) {
        m_capCoreAudioForm->setRowVisible(m_capDevChannelsRow, !isCapPw);
    }
    auto capSuppCh = m_devices->captureConfig.supportedChannels();
    if (!capSuppCh.empty() && !isCapPw) {
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
        m_capDevChannelsSpin->setVisible(!isCapPw);
        m_capDevChannelsSpin->setValue(m_devices->captureConfig.deviceChannels);
    }

    if (isCapPw) {
        m_capStreamChannelsSpin->setRange(1, 32);
    } else {
        int capDevCh = m_devices->captureConfig.deviceChannels;
        m_capStreamChannelsSpin->setRange(1, std::max(1, capDevCh));
    }
    m_capStreamChannelsSpin->setValue(m_devices->captureConfig.channels);

    // Capture Sample Rate
    m_capRateRow->show();
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
    if (m_capCoreAudioForm && m_capFormatRow) {
        m_capCoreAudioForm->setRowVisible(m_capFormatRow, !isCapPw);
    }
    if (isCapPw) {
        m_capFormatCombo->hide();
        m_capFormatLabel->hide();
    } else {
        auto capFormats = m_devices->captureConfig.supportedFormats();
        if (!capFormats.empty()) {
            m_capFormatCombo->show();
            m_capFormatLabel->hide();
            m_capFormatCombo->blockSignals(true);
            m_capFormatCombo->clear();
            m_capFormatCombo->addItem(tr("Auto (Default)"), QString(""));
            for (const auto& fmt : capFormats) {
                m_capFormatCombo->addItem(QString::fromStdString(fmt), QString::fromStdString(fmt));
            }
            if (m_devices->captureConfig.format.has_value() && !m_devices->captureConfig.format->empty() && *m_devices->captureConfig.format != "Auto") {
                int idx = m_capFormatCombo->findData(QString::fromStdString(*m_devices->captureConfig.format));
                if (idx >= 0)
                    m_capFormatCombo->setCurrentIndex(idx);
                else
                    m_capFormatCombo->setCurrentIndex(0);
            } else {
                m_capFormatCombo->setCurrentIndex(0);
            }
            m_capFormatCombo->blockSignals(false);
        } else {
            m_capFormatCombo->hide();
            m_capFormatLabel->show();
            m_capFormatLabel->setText(m_devices->captureConfig.format.has_value() && !m_devices->captureConfig.format->empty() && *m_devices->captureConfig.format != "Auto"
                ? QString::fromStdString(*m_devices->captureConfig.format)
                : tr("Auto (Default)"));
        }
    }

    // Capture DoP
    bool capDopVisible = !isCapPw && isHardwareBackend(m_devices->captureConfig.backend);
    if (m_capCoreAudioForm) {
        m_capCoreAudioForm->setRowVisible(m_bypassDoPCheck, capDopVisible);
        m_capCoreAudioForm->setRowVisible(m_dopCutoffCombo, capDopVisible);
        m_capCoreAudioForm->setRowVisible(m_dopCutoffHint, capDopVisible);
    }

    m_bypassDoPCheck->setChecked(m_devices->captureConfig.bypassDoP);
    m_dopCutoffCombo->setEnabled(!m_devices->captureConfig.bypassDoP);
    if (m_capCoreAudioForm) {
        QWidget* cutoffLbl = m_capCoreAudioForm->labelForField(m_dopCutoffCombo);
        if (cutoffLbl)
            cutoffLbl->setEnabled(!m_devices->captureConfig.bypassDoP);
    }
    int cutoffIdx = m_dopCutoffCombo->findData(m_devices->captureConfig.dopCutoffHz);
    if (cutoffIdx >= 0)
        m_dopCutoffCombo->setCurrentIndex(cutoffIdx);

    m_capWasapiLoopbackCheck->setChecked(m_devices->captureConfig.loopback);
    m_capWasapiExclusiveCheck->setChecked(m_devices->captureConfig.exclusive && !m_devices->captureConfig.loopback);
    m_capWasapiExclusiveCheck->setEnabled(!m_devices->captureConfig.loopback);
    m_capWasapiPollingCheck->setChecked(m_devices->captureConfig.polling);
    m_capAlsaStopInactiveCheck->setChecked(m_devices->captureConfig.stopOnInactive);
    m_capAlsaThreadedCheck->setChecked(m_devices->captureConfig.threaded);

    if (m_capCoreAudioForm) {
        m_capCoreAudioForm->setRowVisible(m_capWasapiExclusiveCheck, isCapWasapi);
        m_capCoreAudioForm->setRowVisible(m_capWasapiLoopbackCheck, isCapWasapi);
        m_capCoreAudioForm->setRowVisible(m_capWasapiPollingCheck, isCapWasapi);
        m_capCoreAudioForm->setRowVisible(m_capAlsaStopInactiveCheck, isCapAlsa);
        m_capCoreAudioForm->setRowVisible(m_capAlsaThreadedCheck, isCapAlsa);
        m_capCoreAudioForm->setRowVisible(m_capAlsaLinkVolumeEdit, isCapAlsa);
        m_capCoreAudioForm->setRowVisible(m_capAlsaLinkMuteEdit, isCapAlsa);
        m_capCoreAudioForm->setRowVisible(m_capPwNodeNameEdit, isCapPw);
        m_capCoreAudioForm->setRowVisible(m_capPwNodeDescEdit, isCapPw);
        m_capCoreAudioForm->setRowVisible(m_capPwNodeGroupEdit, isCapPw);
        m_capCoreAudioForm->setRowVisible(m_capPwAutoconnectEdit, isCapPw);
    }

    if (m_capAlsaLinkVolumeEdit->text().toStdString() != m_devices->captureConfig.linkVolumeControl) {
        m_capAlsaLinkVolumeEdit->blockSignals(true);
        m_capAlsaLinkVolumeEdit->setText(QString::fromStdString(m_devices->captureConfig.linkVolumeControl));
        m_capAlsaLinkVolumeEdit->blockSignals(false);
    }
    if (m_capAlsaLinkMuteEdit->text().toStdString() != m_devices->captureConfig.linkMuteControl) {
        m_capAlsaLinkMuteEdit->blockSignals(true);
        m_capAlsaLinkMuteEdit->setText(QString::fromStdString(m_devices->captureConfig.linkMuteControl));
        m_capAlsaLinkMuteEdit->blockSignals(false);
    }

    if (m_capPwNodeNameEdit->text().toStdString() != m_devices->captureConfig.nodeName) {
        m_capPwNodeNameEdit->blockSignals(true);
        m_capPwNodeNameEdit->setText(QString::fromStdString(m_devices->captureConfig.nodeName));
        m_capPwNodeNameEdit->blockSignals(false);
    }
    if (m_capPwNodeDescEdit->text().toStdString() != m_devices->captureConfig.nodeDescription) {
        m_capPwNodeDescEdit->blockSignals(true);
        m_capPwNodeDescEdit->setText(QString::fromStdString(m_devices->captureConfig.nodeDescription));
        m_capPwNodeDescEdit->blockSignals(false);
    }
    if (m_capPwNodeGroupEdit->text().toStdString() != m_devices->captureConfig.nodeGroupName) {
        m_capPwNodeGroupEdit->blockSignals(true);
        m_capPwNodeGroupEdit->setText(QString::fromStdString(m_devices->captureConfig.nodeGroupName));
        m_capPwNodeGroupEdit->blockSignals(false);
    }
    if (m_capPwAutoconnectEdit->text().toStdString() != m_devices->captureConfig.autoconnectTo) {
        m_capPwAutoconnectEdit->blockSignals(true);
        m_capPwAutoconnectEdit->setText(QString::fromStdString(m_devices->captureConfig.autoconnectTo));
        m_capPwAutoconnectEdit->blockSignals(false);
    }

    // 2. Refresh Capture File & Generator Views
    if (m_capRawFilePathEdit->text().toStdString() != m_devices->captureConfig.filename) {
        m_capRawFilePathEdit->blockSignals(true);
        m_capRawFilePathEdit->setText(QString::fromStdString(m_devices->captureConfig.filename));
        m_capRawFilePathEdit->blockSignals(false);
    }
    m_capRawFileFormatCombo->setCurrentText(QString::fromStdString(m_devices->captureConfig.fileFormat));
    m_capRawFileChannelsSpin->setValue(m_devices->captureConfig.channels);
    m_capRawSkipBytesSpin->setValue(m_devices->captureConfig.skipBytes);
    m_capRawReadBytesSpin->setValue(m_devices->captureConfig.readBytes);
    m_capRawExtraSamplesSpin->setValue(m_devices->captureConfig.extraSamples);

    if (m_capWavFilePathEdit->text().toStdString() != m_devices->captureConfig.filename) {
        m_capWavFilePathEdit->blockSignals(true);
        m_capWavFilePathEdit->setText(QString::fromStdString(m_devices->captureConfig.filename));
        m_capWavFilePathEdit->blockSignals(false);
    }
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
    m_genFreqSpin->setEnabled(!isNoise);
    m_genFreqSlider->setEnabled(!isNoise);

    // 3. Refresh Playback Devices List & CoreAudio controls
    if (!isPbPw) {
        populateDeviceList(m_pbDeviceList, m_pbWarningLabel, m_devices->playbackDevices,
                           m_devices->playbackConfig.deviceName());
    } else {
        m_pbWarningLabel->hide();
        m_pbDeviceList->hide();
    }
    if (m_pbCoreAudioForm && m_pbDeviceContainer) {
        m_pbCoreAudioForm->setRowVisible(m_pbDeviceContainer, !isPbPw);
    }

    int pbBackendIdx = m_pbBackendCombo->findData(static_cast<int>(m_devices->playbackConfig.backend));
    if (pbBackendIdx >= 0) {
        m_pbBackendCombo->blockSignals(true);
        m_pbBackendCombo->setCurrentIndex(pbBackendIdx);
        m_pbBackendCombo->blockSignals(false);
    }
    m_pbStack->setCurrentIndex(getPbStackIndex(m_devices->playbackConfig.backend));

    // Playback Channels
    if (m_pbCoreAudioForm && m_pbDevChannelsRow) {
        m_pbCoreAudioForm->setRowVisible(m_pbDevChannelsRow, !isPbPw);
    }
    auto pbSuppCh = m_devices->playbackConfig.supportedChannels();
    if (!pbSuppCh.empty() && !isPbPw) {
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
        m_pbDevChannelsSpin->setVisible(!isPbPw);
        m_pbDevChannelsSpin->setValue(m_devices->playbackConfig.deviceChannels);
    }

    if (isPbPw) {
        m_pbStreamChannelsSpin->setRange(1, 32);
    } else {
        int pbDevCh = m_devices->playbackConfig.deviceChannels;
        m_pbStreamChannelsSpin->setRange(1, std::max(1, pbDevCh));
    }
    m_pbStreamChannelsSpin->setValue(m_devices->playbackConfig.channels);

    // Playback Sample Rate
    m_pbRateCombo->show();
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
    if (m_pbCoreAudioForm && m_pbFormatRow) {
        m_pbCoreAudioForm->setRowVisible(m_pbFormatRow, !isPbPw);
    }
    if (isPbPw) {
        m_pbFormatCombo->hide();
        m_pbFormatLabel->hide();
    } else {
        auto pbFormats = m_devices->playbackConfig.supportedFormats();
        if (!pbFormats.empty()) {
            m_pbFormatCombo->show();
            m_pbFormatLabel->hide();
            m_pbFormatCombo->blockSignals(true);
            m_pbFormatCombo->clear();
            m_pbFormatCombo->addItem(tr("Auto (Default)"), QString(""));
            for (const auto& fmt : pbFormats) {
                m_pbFormatCombo->addItem(QString::fromStdString(fmt), QString::fromStdString(fmt));
            }
            if (m_devices->playbackConfig.format.has_value() && !m_devices->playbackConfig.format->empty() && *m_devices->playbackConfig.format != "Auto") {
                int idx = m_pbFormatCombo->findData(QString::fromStdString(*m_devices->playbackConfig.format));
                if (idx >= 0)
                    m_pbFormatCombo->setCurrentIndex(idx);
                else
                    m_pbFormatCombo->setCurrentIndex(0);
            } else {
                m_pbFormatCombo->setCurrentIndex(0);
            }
            m_pbFormatCombo->blockSignals(false);
        } else {
            m_pbFormatCombo->hide();
            m_pbFormatLabel->show();
            m_pbFormatLabel->setText(m_devices->playbackConfig.format.has_value() && !m_devices->playbackConfig.format->empty() && *m_devices->playbackConfig.format != "Auto"
                ? QString::fromStdString(*m_devices->playbackConfig.format)
                : tr("Auto (Default)"));
        }
    }

    bool pbExclusiveVisible = isPbWasapi || isPbCoreAudio;
    m_exclusiveModeCheck->setChecked(m_devices->playbackConfig.exclusive);

    m_pbWasapiPollingCheck->setChecked(m_devices->playbackConfig.polling);

    m_pbAlsaThreadedCheck->setChecked(m_devices->playbackConfig.threaded);

    bool pbDopVisible = !isPbPw && isHardwareBackend(m_devices->playbackConfig.backend);
    m_outputDoPCheck->setChecked(m_devices->playbackConfig.outputDoP);

    int filterIdx = m_sdmFilterCombo->findData(static_cast<int>(m_devices->playbackConfig.dsdEncoderFilter));
    if (filterIdx >= 0)
        m_sdmFilterCombo->setCurrentIndex(filterIdx);

    if (m_pbCoreAudioForm) {
        m_pbCoreAudioForm->setRowVisible(m_exclusiveModeCheck, pbExclusiveVisible);
        m_pbCoreAudioForm->setRowVisible(m_exclusiveModeHint, pbExclusiveVisible);
        m_pbCoreAudioForm->setRowVisible(m_pbWasapiPollingCheck, isPbWasapi);
        m_pbCoreAudioForm->setRowVisible(m_pbAlsaThreadedCheck, isPbAlsa);
        m_pbCoreAudioForm->setRowVisible(m_pbPwNodeNameEdit, isPbPw);
        m_pbCoreAudioForm->setRowVisible(m_pbPwNodeDescEdit, isPbPw);
        m_pbCoreAudioForm->setRowVisible(m_pbPwNodeGroupEdit, isPbPw);
        m_pbCoreAudioForm->setRowVisible(m_pbPwAutoconnectEdit, isPbPw);
        m_pbCoreAudioForm->setRowVisible(m_outputDoPCheck, pbDopVisible);
        m_pbCoreAudioForm->setRowVisible(m_sdmFilterCombo, pbDopVisible);
        m_pbCoreAudioForm->setRowVisible(m_pbDopHintLabel, pbDopVisible);
    }

    if (m_pbPwNodeNameEdit->text().toStdString() != m_devices->playbackConfig.nodeName) {
        m_pbPwNodeNameEdit->blockSignals(true);
        m_pbPwNodeNameEdit->setText(QString::fromStdString(m_devices->playbackConfig.nodeName));
        m_pbPwNodeNameEdit->blockSignals(false);
    }
    if (m_pbPwNodeDescEdit->text().toStdString() != m_devices->playbackConfig.nodeDescription) {
        m_pbPwNodeDescEdit->blockSignals(true);
        m_pbPwNodeDescEdit->setText(QString::fromStdString(m_devices->playbackConfig.nodeDescription));
        m_pbPwNodeDescEdit->blockSignals(false);
    }
    if (m_pbPwNodeGroupEdit->text().toStdString() != m_devices->playbackConfig.nodeGroupName) {
        m_pbPwNodeGroupEdit->blockSignals(true);
        m_pbPwNodeGroupEdit->setText(QString::fromStdString(m_devices->playbackConfig.nodeGroupName));
        m_pbPwNodeGroupEdit->blockSignals(false);
    }
    if (m_pbPwAutoconnectEdit->text().toStdString() != m_devices->playbackConfig.autoconnectTo) {
        m_pbPwAutoconnectEdit->blockSignals(true);
        m_pbPwAutoconnectEdit->setText(QString::fromStdString(m_devices->playbackConfig.autoconnectTo));
        m_pbPwAutoconnectEdit->blockSignals(false);
    }

    updateDoPCapability();

    // 4. Refresh Playback File View
    if (m_pbRawFilePathEdit && m_pbRawFilePathEdit->text().toStdString() != m_devices->playbackConfig.filename) {
        m_pbRawFilePathEdit->blockSignals(true);
        m_pbRawFilePathEdit->setText(QString::fromStdString(m_devices->playbackConfig.filename));
        m_pbRawFilePathEdit->blockSignals(false);
    }
    if (m_pbRawFileFormatCombo)
        m_pbRawFileFormatCombo->setCurrentText(QString::fromStdString(m_devices->playbackConfig.fileFormat));
    if (m_pbRawFileChannelsSpin)
        m_pbRawFileChannelsSpin->setValue(m_devices->playbackConfig.channels);

    if (m_pbWavFilePathEdit && m_pbWavFilePathEdit->text().toStdString() != m_devices->playbackConfig.filename) {
        m_pbWavFilePathEdit->blockSignals(true);
        m_pbWavFilePathEdit->setText(QString::fromStdString(m_devices->playbackConfig.filename));
        m_pbWavFilePathEdit->blockSignals(false);
    }
    if (m_pbWavFileFormatCombo) {
        QString fmt = QString::fromStdString(m_devices->playbackConfig.fileFormat);
        if (fmt == "S24_4_RJ_LE") {
            fmt = "S16_LE";
            m_devices->playbackConfig.fileFormat = "S16_LE";
        }
        m_pbWavFileFormatCombo->setCurrentText(fmt);
    }
    if (m_pbWavFileChannelsSpin)
        m_pbWavFileChannelsSpin->setValue(m_devices->playbackConfig.channels);
    if (m_pbWavUseRf64Combo) {
        int idx = m_pbWavUseRf64Combo->findData(m_devices->playbackConfig.useRf64);
        if (idx >= 0)
            m_pbWavUseRf64Combo->setCurrentIndex(idx);
    }

    // 5. Refresh Processing Settings
    int chunkIdx = m_chunkSizeCombo->findData(m_settings->chunkSize);
    if (chunkIdx >= 0)
        m_chunkSizeCombo->setCurrentIndex(chunkIdx);
    updateLatencyText();

    m_enableRateAdjustCheck->setChecked(m_settings->enableRateAdjust);
    m_queueLimitSpin->setValue(m_settings->queuelimit);
    m_stopOnRateChangeCheck->setChecked(m_settings->stopOnRateChange);

    m_measureIntervalSlider->setValue(static_cast<int>(m_settings->rateMeasureInterval * 10.0));
    m_measureIntervalValLabel->setText(QString("%1 s").arg(m_settings->rateMeasureInterval, 0, 'f', 1));

    m_multithreadedCheck->setChecked(m_settings->multithreaded);
    if (m_procForm) {
        m_procForm->setRowVisible(m_workerThreadsSpin, m_settings->multithreaded);
    }
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
    if (m_measureIntervalSlider)
        m_settings->rateMeasureInterval = m_measureIntervalSlider->value() / 10.0;
    if (m_multithreadedCheck)
        m_settings->multithreaded = m_multithreadedCheck->isChecked();
    if (m_workerThreadsSpin)
        m_settings->workerThreads = m_workerThreadsSpin->value();
    m_settings->savePreferences();

    // 2. Playback settings (resolved first so capture can sync sample rate if non-resampling)
    DeviceConfig pbCfg = m_devices->playbackConfig;
    if (m_pbBackendCombo->currentIndex() >= 0) {
        pbCfg.backend = static_cast<AudioBackendType>(m_pbBackendCombo->currentData().toInt());
    }

    if (isHardwareBackend(pbCfg.backend)) {
#if defined(ENABLE_PIPEWIRE)
        if (pbCfg.backend == AudioBackendType::PipeWire) {
            pbCfg.channels = m_pbStreamChannelsSpin->value();
            pbCfg.deviceChannels = pbCfg.channels;
            if (m_pbRateCombo->currentIndex() >= 0) {
                pbCfg.sampleRate = m_pbRateCombo->currentData().toInt();
            }
        } else
#endif
        {
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
            if (!pbFormats.empty() && m_pbFormatCombo->isVisible() && m_pbFormatCombo->currentIndex() >= 0) {
                QString sel = m_pbFormatCombo->currentData().toString();
                if (sel.isEmpty() || sel == "Auto") {
                    pbCfg.format = std::nullopt;
                } else {
                    pbCfg.format = sel.toStdString();
                }
            } else {
                pbCfg.format = std::nullopt;
            }
        }

        pbCfg.exclusive = m_exclusiveModeCheck->isChecked();
        pbCfg.polling = m_pbWasapiPollingCheck->isChecked();
        pbCfg.threaded = m_pbAlsaThreadedCheck->isChecked();
        if (m_pbPwNodeNameEdit)
            pbCfg.nodeName = m_pbPwNodeNameEdit->text().toStdString();
        if (m_pbPwNodeDescEdit)
            pbCfg.nodeDescription = m_pbPwNodeDescEdit->text().toStdString();
        if (m_pbPwNodeGroupEdit)
            pbCfg.nodeGroupName = m_pbPwNodeGroupEdit->text().toStdString();
        if (m_pbPwAutoconnectEdit)
            pbCfg.autoconnectTo = m_pbPwAutoconnectEdit->text().toStdString();
        pbCfg.outputDoP = m_outputDoPCheck->isChecked();
        if (m_sdmFilterCombo->currentIndex() >= 0) {
            pbCfg.dsdEncoderFilter = static_cast<SDMFilter>(m_sdmFilterCombo->currentData().toInt());
        }
    } else if (pbCfg.backend == AudioBackendType::RawFile) {
        if (m_pbRawFilePathEdit)
            pbCfg.filename = m_pbRawFilePathEdit->text().toStdString();
        if (m_pbRawFileFormatCombo)
            pbCfg.fileFormat = m_pbRawFileFormatCombo->currentText().toStdString();
        if (m_pbRawFileChannelsSpin)
            pbCfg.channels = m_pbRawFileChannelsSpin->value();
        pbCfg.deviceChannels = pbCfg.channels;
        pbCfg.isWav = false;
    } else if (pbCfg.backend == AudioBackendType::WavFile) {
        if (m_pbWavFilePathEdit)
            pbCfg.filename = m_pbWavFilePathEdit->text().toStdString();
        if (m_pbWavFileFormatCombo) {
            std::string fmt = m_pbWavFileFormatCombo->currentText().toStdString();
            if (fmt == "S24_4_RJ_LE")
                fmt = "S16_LE";
            pbCfg.fileFormat = fmt;
        }
        if (m_pbWavFileChannelsSpin)
            pbCfg.channels = m_pbWavFileChannelsSpin->value();
        pbCfg.deviceChannels = pbCfg.channels;
        pbCfg.isWav = true;
        if (m_pbWavUseRf64Combo && m_pbWavUseRf64Combo->currentIndex() >= 0)
            pbCfg.useRf64 = m_pbWavUseRf64Combo->currentData().toBool();
    }
    m_devices->setPlaybackConfig(pbCfg);

    // 3. Capture settings
    DeviceConfig capCfg = m_devices->captureConfig;
    if (m_capBackendCombo->currentIndex() >= 0) {
        capCfg.backend = static_cast<AudioBackendType>(m_capBackendCombo->currentData().toInt());
    }

    if (isHardwareBackend(capCfg.backend)) {
#if defined(ENABLE_PIPEWIRE)
        if (capCfg.backend == AudioBackendType::PipeWire) {
            capCfg.channels = m_capStreamChannelsSpin->value();
            capCfg.deviceChannels = capCfg.channels;
            if (m_settings->resamplerEnabled && m_capRateCombo->isVisible() && m_capRateCombo->currentIndex() >= 0) {
                capCfg.sampleRate = m_capRateCombo->currentData().toInt();
            } else if (!m_settings->resamplerEnabled) {
                capCfg.sampleRate = pbCfg.sampleRate;
            }
        } else
#endif
        {
            auto capSuppCh = capCfg.supportedChannels();
            if (!capSuppCh.empty() && m_capDevChannelsCombo->currentIndex() >= 0) {
                capCfg.deviceChannels = m_capDevChannelsCombo->currentData().toInt();
            } else {
                capCfg.deviceChannels = m_capDevChannelsSpin->value();
            }
            capCfg.channels = m_capStreamChannelsSpin->value();

            if (m_settings->resamplerEnabled && m_capRateCombo->isVisible() && m_capRateCombo->currentIndex() >= 0) {
                capCfg.sampleRate = m_capRateCombo->currentData().toInt();
            } else if (!m_settings->resamplerEnabled) {
                capCfg.sampleRate = pbCfg.sampleRate;
            }

            auto capFormats = capCfg.supportedFormats();
            if (!capFormats.empty() && m_capFormatCombo->isVisible() && m_capFormatCombo->currentIndex() >= 0) {
                QString sel = m_capFormatCombo->currentData().toString();
                if (sel.isEmpty() || sel == "Auto") {
                    capCfg.format = std::nullopt;
                } else {
                    capCfg.format = sel.toStdString();
                }
            } else {
                capCfg.format = std::nullopt;
            }
        }

        capCfg.bypassDoP = m_bypassDoPCheck->isChecked();
        if (m_dopCutoffCombo->currentIndex() >= 0) {
            capCfg.dopCutoffHz = m_dopCutoffCombo->currentData().toDouble();
        }
        capCfg.loopback = m_capWasapiLoopbackCheck->isChecked();
        capCfg.exclusive = capCfg.loopback ? false : m_capWasapiExclusiveCheck->isChecked();
        capCfg.polling = m_capWasapiPollingCheck->isChecked();
        capCfg.stopOnInactive = m_capAlsaStopInactiveCheck->isChecked();
        capCfg.threaded = m_capAlsaThreadedCheck->isChecked();
        if (m_capAlsaLinkVolumeEdit)
            capCfg.linkVolumeControl = m_capAlsaLinkVolumeEdit->text().toStdString();
        if (m_capAlsaLinkMuteEdit)
            capCfg.linkMuteControl = m_capAlsaLinkMuteEdit->text().toStdString();
        if (m_capPwNodeNameEdit)
            capCfg.nodeName = m_capPwNodeNameEdit->text().toStdString();
        if (m_capPwNodeDescEdit)
            capCfg.nodeDescription = m_capPwNodeDescEdit->text().toStdString();
        if (m_capPwNodeGroupEdit)
            capCfg.nodeGroupName = m_capPwNodeGroupEdit->text().toStdString();
        if (m_capPwAutoconnectEdit)
            capCfg.autoconnectTo = m_capPwAutoconnectEdit->text().toStdString();
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

    m_devices->setExclusiveMode(m_exclusiveModeCheck->isChecked());
    if (m_devices->onConfigChanged) {
        m_devices->onConfigChanged();
    }
}
