#include "ui/DevicePickerView.h"

#include "ui/StyleTheme.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace {

class DeviceRowWidget : public QFrame {
public:
    DeviceRowWidget(const QString& name, bool isSelected, std::function<void()> onSelect, QWidget* parent = nullptr)
        : QFrame(parent), m_onSelect(onSelect) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);

        auto layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 6, 8, 6);
        layout->setSpacing(8);

        auto leftCheck = new QLabel(isSelected ? "●" : "○", this);
        leftCheck->setStyleSheet(isSelected ? "color: #007af5; font-size: 13px;" : "color: #8e8e93; font-size: 13px;");
        leftCheck->setFixedWidth(16);
        layout->addWidget(leftCheck);

        auto textLbl = new QLabel(name, this);
        if (isSelected) {
            QFont f = textLbl->font();
            f.setBold(true);
            textLbl->setFont(f);
            textLbl->setStyleSheet("color: #007af5;");
        } else {
            textLbl->setStyleSheet("color: #1c1c1e;");
        }
        layout->addWidget(textLbl);
        layout->addStretch();

        if (isSelected) {
            auto rightCheck = new QLabel("✓", this);
            rightCheck->setStyleSheet("color: #007af5; font-weight: bold; font-size: 12px;");
            layout->addWidget(rightCheck);
        }

        if (isSelected) {
            setStyleSheet("DeviceRowWidget { background-color: rgba(0, 122, 245, 0.08); border-radius: 4px; }");
        } else {
            setStyleSheet("DeviceRowWidget { background-color: transparent; border-radius: 4px; }"
                          "DeviceRowWidget:hover { background-color: rgba(0, 0, 0, 0.04); }");
        }
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        QFrame::mousePressEvent(event);
        if (m_onSelect) {
            m_onSelect();
        }
    }

private:
    std::function<void()> m_onSelect;
};

} // namespace

DevicePickerView::DevicePickerView(std::shared_ptr<AudioDeviceManager> devices, std::shared_ptr<AudioSettings> settings,
                                   QWidget* parent)
    : QWidget(parent), m_devices(devices), m_settings(settings) {
    setupUi();

    connect(m_devices.get(), &AudioDeviceManager::devicesRefreshed, this, &DevicePickerView::refreshUi,
            Qt::QueuedConnection);
    connect(m_devices.get(), &AudioDeviceManager::configChanged, this, &DevicePickerView::refreshUi,
            Qt::QueuedConnection);

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

    // 1. Capture Group
    auto capGroup = new QGroupBox("Capture (Input)", container);
    auto capLayout = new QVBoxLayout(capGroup);

    auto capHeaderLabel = new QLabel("🎤  Capture (Input)", capGroup);
    capHeaderLabel->setFont(QFont("sans-serif", 13, QFont::Bold));
    capHeaderLabel->setStyleSheet("color: #007af5;");
    capLayout->addWidget(capHeaderLabel);

    auto capBackendBox = new QHBoxLayout();
    auto capBackendLbl = new QLabel("Backend", capGroup);
    capBackendLbl->setFixedWidth(100);
    capBackendBox->addWidget(capBackendLbl);

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
    capBackendBox->addWidget(m_capBackendCombo);
    capBackendBox->addStretch();
    capLayout->addLayout(capBackendBox);

    auto capDiv = new QFrame(capGroup);
    capDiv->setFrameShape(QFrame::HLine);
    capDiv->setFrameShadow(QFrame::Sunken);
    capDiv->setStyleSheet("background-color: #e5e5ea; max-height: 1px; margin-top: 4px; margin-bottom: 4px;");
    capLayout->addWidget(capDiv);

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

    auto pbHeaderLabel = new QLabel("🔊  Playback (Output)", pbGroup);
    pbHeaderLabel->setFont(QFont("sans-serif", 13, QFont::Bold));
    pbHeaderLabel->setStyleSheet("color: #34c759;");
    pbLayout->addWidget(pbHeaderLabel);

    auto pbBackendBox = new QHBoxLayout();
    auto pbBackendLbl = new QLabel("Backend", pbGroup);
    pbBackendLbl->setFixedWidth(100);
    pbBackendBox->addWidget(pbBackendLbl);

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
    pbBackendBox->addWidget(m_pbBackendCombo);
    pbBackendBox->addStretch();
    pbLayout->addLayout(pbBackendBox);

    auto pbDiv = new QFrame(pbGroup);
    pbDiv->setFrameShape(QFrame::HLine);
    pbDiv->setFrameShadow(QFrame::Sunken);
    pbDiv->setStyleSheet("background-color: #e5e5ea; max-height: 1px; margin-top: 4px; margin-bottom: 4px;");
    pbLayout->addWidget(pbDiv);

    m_pbStack = new QStackedWidget(pbGroup);
    m_pbStack->addWidget(createPbCoreAudioView());
    m_pbStack->addWidget(createPbFileView(false));
    m_pbStack->addWidget(createPbFileView(true));
    pbLayout->addWidget(m_pbStack);

    mainLayout->addWidget(pbGroup);

    // 3. Processing Group
    auto procGroup = new QGroupBox("Processing", container);
    auto procLayout = new QVBoxLayout(procGroup);

    auto procHeaderLabel = new QLabel("⚙️  Processing", procGroup);
    procHeaderLabel->setFont(QFont("sans-serif", 13, QFont::Bold));
    procLayout->addWidget(procHeaderLabel);

    auto procForm = new QFormLayout();
    procForm->setSpacing(12);

    auto chunkLayout = new QHBoxLayout();
    auto chunkLbl = new QLabel("Chunk Size", procGroup);
    chunkLbl->setFixedWidth(100);
    chunkLayout->addWidget(chunkLbl);

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
    procForm->addRow("", chunkLayout);

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

    auto procDiv = new QFrame(procGroup);
    procDiv->setFrameShape(QFrame::HLine);
    procDiv->setFrameShadow(QFrame::Sunken);
    procDiv->setStyleSheet("background-color: #e5e5ea; max-height: 1px; margin-top: 8px; margin-bottom: 8px;");
    procForm->addRow("", procDiv);

    auto qlLayout = new QHBoxLayout();
    auto qlLbl = new QLabel("Queue Limit", procGroup);
    qlLbl->setFixedWidth(120);
    qlLayout->addWidget(qlLbl);

    m_queueLimitSpin = new QSpinBox(procGroup);
    m_queueLimitSpin->setRange(1, 32);
    m_queueLimitSpin->setMinimumWidth(110);
    m_queueLimitSpin->setMaximumWidth(140);
    connect(m_queueLimitSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    qlLayout->addWidget(m_queueLimitSpin);
    qlLayout->addStretch();
    procForm->addRow("", qlLayout);

    m_stopOnRateChangeCheck = new QCheckBox("Stop on Rate Change", procGroup);
    connect(m_stopOnRateChangeCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    procForm->addRow("", m_stopOnRateChangeCheck);

    auto intervalBox = new QHBoxLayout();
    auto miLbl = new QLabel("Measure Interval", procGroup);
    miLbl->setFixedWidth(120);
    intervalBox->addWidget(miLbl);

    m_measureIntervalSlider = new QSlider(Qt::Horizontal, procGroup);
    m_measureIntervalSlider->setRange(1, 100); // 0.1 to 10.0 s
    m_measureIntervalSlider->setFixedWidth(150);

    m_measureIntervalValLabel = new QLabel(procGroup);
    m_measureIntervalValLabel->setFont(QFont("monospace", 11));
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
    procForm->addRow("", intervalBox);

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
    m_workerThreadsSpin->setMinimumWidth(110);
    m_workerThreadsSpin->setMaximumWidth(140);
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

void DevicePickerView::populateDeviceList(QVBoxLayout* listLayout, QWidget* warningWidget, QWidget* containerWidget,
                                          const std::vector<AudioDevice>& devices,
                                          const std::optional<std::string>& selectedDeviceName, bool isCapture) {
    if (!listLayout || !warningWidget || !containerWidget)
        return;

    QLayoutItem* item;
    while ((item = listLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    if (devices.empty()) {
        warningWidget->show();
        containerWidget->hide();
        return;
    }

    warningWidget->hide();
    containerWidget->show();

    bool defaultSelected = !selectedDeviceName.has_value() || selectedDeviceName.value().empty();
    auto defaultRow = new DeviceRowWidget(
        "System Default", defaultSelected,
        [this, isCapture]() {
            if (m_isRefreshing)
                return;
            if (isCapture) {
                m_devices->captureConfig.setDeviceName("");
            } else {
                m_devices->playbackConfig.setDeviceName("");
            }
            m_devices->refreshDeviceCapabilities();
            m_devices->validateSampleRates();
            applySettings();
            refreshUi();
        },
        containerWidget);
    listLayout->addWidget(defaultRow);

    auto div = new QFrame(containerWidget);
    div->setFrameShape(QFrame::HLine);
    div->setFrameShadow(QFrame::Sunken);
    div->setStyleSheet("background-color: #e5e5ea; max-height: 1px; margin-top: 4px; margin-bottom: 4px;");
    listLayout->addWidget(div);

    for (const auto& dev : devices) {
        bool isSelected = selectedDeviceName.has_value() && !selectedDeviceName.value().empty() &&
                          (selectedDeviceName.value() == dev.id || selectedDeviceName.value() == dev.name);
        std::string devId = dev.id;
        std::string devDisplay;
        if (dev.id.empty() || dev.id == dev.name) {
            devDisplay = dev.name;
        } else {
            devDisplay = dev.id + " (" + dev.name + ")";
        }
        auto row = new DeviceRowWidget(
            QString::fromStdString(devDisplay), isSelected,
            [this, isCapture, devId]() {
                if (m_isRefreshing)
                    return;
                if (isCapture) {
                    m_devices->captureConfig.setDeviceName(devId);
                } else {
                    m_devices->playbackConfig.setDeviceName(devId);
                }
                m_devices->refreshDeviceCapabilities();
                m_devices->validateSampleRates();
                applySettings();
                refreshUi();
            },
            containerWidget);
        listLayout->addWidget(row);
    }
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

    m_capDeviceListContainer = new QWidget(w);
    m_capDeviceListLayout = new QVBoxLayout(m_capDeviceListContainer);
    m_capDeviceListLayout->setContentsMargins(0, 0, 0, 0);
    m_capDeviceListLayout->setSpacing(2);

    auto devBox = new QVBoxLayout();
    devBox->addWidget(m_capWarningWidget);
    devBox->addWidget(m_capDeviceListContainer);
    form->addRow("", devBox);

    auto chBox = new QHBoxLayout();
    m_capDevChannelsCombo = new QComboBox(w);
    m_capDevChannelsSpin = new QSpinBox(w);
    m_capDevChannelsSpin->setRange(1, 32);
    m_capDevChannelsSpin->setMinimumWidth(110);
    m_capDevChannelsSpin->setMaximumWidth(140);

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

    m_capDevChannelsLabel = new QLabel("Device Channels", w);
    m_capDevChannelsLabel->setFixedWidth(110);
    chBox->addWidget(m_capDevChannelsLabel);
    chBox->addWidget(m_capDevChannelsCombo);
    chBox->addWidget(m_capDevChannelsSpin);

    m_capStreamChannelsSpin = new QSpinBox(w);
    m_capStreamChannelsSpin->setRange(1, 32);
    m_capStreamChannelsSpin->setMinimumWidth(110);
    m_capStreamChannelsSpin->setMaximumWidth(140);
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

    m_capRateRow = new QWidget(w);
    auto rateBox = new QHBoxLayout(m_capRateRow);
    rateBox->setContentsMargins(0, 0, 0, 0);
    auto rateLbl = new QLabel("Sample Rate", m_capRateRow);
    rateLbl->setFixedWidth(100);
    rateBox->addWidget(rateLbl);

    m_capRateCombo = new QComboBox(m_capRateRow);
    connect(m_capRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() { applySettings(); });
    });

    m_capRateLabel = new QLabel(m_capRateRow);
    m_capRateLabel->setStyleSheet("font-family: monospace; color: #8e8e93; font-size: 13px;");

    rateBox->addWidget(m_capRateCombo);
    rateBox->addWidget(m_capRateLabel);
    rateBox->addStretch();
    form->addRow("", m_capRateRow);

    m_capFormatRow = new QWidget(w);
    auto fmtBox = new QHBoxLayout(m_capFormatRow);
    fmtBox->setContentsMargins(0, 0, 0, 0);
    auto fmtLbl = new QLabel("Format", m_capFormatRow);
    fmtLbl->setFixedWidth(100);
    fmtBox->addWidget(fmtLbl);

    m_capFormatCombo = new QComboBox(m_capFormatRow);
    connect(m_capFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() { applySettings(); });
    });

    m_capFormatLabel = new QLabel(m_capFormatRow);
    m_capFormatLabel->setStyleSheet("font-family: monospace; color: #8e8e93; font-size: 13px;");

    fmtBox->addWidget(m_capFormatCombo);
    fmtBox->addWidget(m_capFormatLabel);
    fmtBox->addStretch();
    form->addRow("", m_capFormatRow);

    m_capDopDivider = new QFrame(w);
    m_capDopDivider->setFrameShape(QFrame::HLine);
    m_capDopDivider->setFrameShadow(QFrame::Sunken);
    m_capDopDivider->setStyleSheet("background-color: #e5e5ea; max-height: 1px; margin-top: 4px; margin-bottom: 4px;");
    form->addRow("", m_capDopDivider);

    m_bypassDoPCheck = new QCheckBox("Bypass DoP Detection", w);
    connect(m_bypassDoPCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_isRefreshing)
            return;
        m_dopCutoffLabel->setEnabled(!checked);
        m_dopCutoffCombo->setEnabled(!checked);
        applySettings();
    });
    form->addRow("", m_bypassDoPCheck);

    m_capDopCutoffRow = new QWidget(w);
    auto cutoffLayout = new QHBoxLayout(m_capDopCutoffRow);
    cutoffLayout->setContentsMargins(0, 0, 0, 0);
    m_dopCutoffLabel = new QLabel("DoP Cutoff", m_capDopCutoffRow);
    m_dopCutoffLabel->setFixedWidth(100);
    cutoffLayout->addWidget(m_dopCutoffLabel);

    m_dopCutoffCombo = new QComboBox(m_capDopCutoffRow);
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
    cutoffLayout->addWidget(m_dopCutoffCombo);
    cutoffLayout->addStretch();
    form->addRow("", m_capDopCutoffRow);

    m_dopCutoffHint = new QLabel("Lower cutoff = higher SINAD; higher cutoff preserves more ultrasonic content", w);
    m_dopCutoffHint->setStyleSheet("color: #8e8e93; font-size: 11px;");
    form->addRow("", m_dopCutoffHint);

    m_capWasapiExclusiveCheck = new QCheckBox("WASAPI Exclusive Mode", w);
    connect(m_capWasapiExclusiveCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    form->addRow("", m_capWasapiExclusiveCheck);

    m_capWasapiLoopbackCheck = new QCheckBox("WASAPI Loopback (Record Output Stream)", w);
    connect(m_capWasapiLoopbackCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    form->addRow("", m_capWasapiLoopbackCheck);

    m_capWasapiPollingCheck = new QCheckBox("WASAPI Polling Mode", w);
    connect(m_capWasapiPollingCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    form->addRow("", m_capWasapiPollingCheck);

    m_capAlsaStopInactiveCheck = new QCheckBox("Stop Streams When Inactive", w);
    connect(m_capAlsaStopInactiveCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    form->addRow("", m_capAlsaStopInactiveCheck);

    m_capAlsaThreadedCheck = new QCheckBox("Threaded Ring Buffer Mode", w);
    connect(m_capAlsaThreadedCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    form->addRow("", m_capAlsaThreadedCheck);

    m_capAlsaLinkVolRow = new QWidget(w);
    auto capVolLayout = new QHBoxLayout(m_capAlsaLinkVolRow);
    capVolLayout->setContentsMargins(0, 0, 0, 0);
    auto capVolLbl = new QLabel("Link Volume Control", m_capAlsaLinkVolRow);
    capVolLbl->setFixedWidth(130);
    capVolLayout->addWidget(capVolLbl);
    m_capAlsaLinkVolumeEdit = new QLineEdit(m_capAlsaLinkVolRow);
    m_capAlsaLinkVolumeEdit->setPlaceholderText("e.g. Master");
    connect(m_capAlsaLinkVolumeEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    capVolLayout->addWidget(m_capAlsaLinkVolumeEdit);
    form->addRow("", m_capAlsaLinkVolRow);

    m_capAlsaLinkMuteRow = new QWidget(w);
    auto capMuteLayout = new QHBoxLayout(m_capAlsaLinkMuteRow);
    capMuteLayout->setContentsMargins(0, 0, 0, 0);
    auto capMuteLbl = new QLabel("Link Mute Control", m_capAlsaLinkMuteRow);
    capMuteLbl->setFixedWidth(130);
    capMuteLayout->addWidget(capMuteLbl);
    m_capAlsaLinkMuteEdit = new QLineEdit(m_capAlsaLinkMuteRow);
    m_capAlsaLinkMuteEdit->setPlaceholderText("e.g. Master");
    connect(m_capAlsaLinkMuteEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    capMuteLayout->addWidget(m_capAlsaLinkMuteEdit);
    form->addRow("", m_capAlsaLinkMuteRow);

    m_capPipeWireRow = new QWidget(w);
    auto capPwLayout = new QVBoxLayout(m_capPipeWireRow);
    capPwLayout->setContentsMargins(0, 0, 0, 0);
    capPwLayout->setSpacing(8);

    auto pwNameBox = new QHBoxLayout();
    auto pwNameLbl = new QLabel("Node Name", m_capPipeWireRow);
    pwNameLbl->setFixedWidth(130);
    pwNameBox->addWidget(pwNameLbl);
    m_capPwNodeNameEdit = new QLineEdit(m_capPipeWireRow);
    m_capPwNodeNameEdit->setPlaceholderText("e.g. CamillaDSP");
    connect(m_capPwNodeNameEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    pwNameBox->addWidget(m_capPwNodeNameEdit);
    capPwLayout->addLayout(pwNameBox);

    auto pwDescBox = new QHBoxLayout();
    auto pwDescLbl = new QLabel("Node Description", m_capPipeWireRow);
    pwDescLbl->setFixedWidth(130);
    pwDescBox->addWidget(pwDescLbl);
    m_capPwNodeDescEdit = new QLineEdit(m_capPipeWireRow);
    m_capPwNodeDescEdit->setPlaceholderText("e.g. CamillaDSP Capture");
    connect(m_capPwNodeDescEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    pwDescBox->addWidget(m_capPwNodeDescEdit);
    capPwLayout->addLayout(pwDescBox);

    auto pwGroupBox = new QHBoxLayout();
    auto pwGroupLbl = new QLabel("Node Group", m_capPipeWireRow);
    pwGroupLbl->setFixedWidth(130);
    pwGroupBox->addWidget(pwGroupLbl);
    m_capPwNodeGroupEdit = new QLineEdit(m_capPipeWireRow);
    connect(m_capPwNodeGroupEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    pwGroupBox->addWidget(m_capPwNodeGroupEdit);
    capPwLayout->addLayout(pwGroupBox);

    auto pwAutoBox = new QHBoxLayout();
    auto pwAutoLbl = new QLabel("Autoconnect To", m_capPipeWireRow);
    pwAutoLbl->setFixedWidth(130);
    pwAutoBox->addWidget(pwAutoLbl);
    m_capPwAutoconnectEdit = new QLineEdit(m_capPipeWireRow);
    connect(m_capPwAutoconnectEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    pwAutoBox->addWidget(m_capPwAutoconnectEdit);
    capPwLayout->addLayout(pwAutoBox);

    form->addRow("", m_capPipeWireRow);

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
        m_capWavFilePathEdit->setClearButtonEnabled(true);
        connect(m_capWavFilePathEdit, &QLineEdit::returnPressed, [this]() { applySettings(); });
        connect(m_capWavFilePathEdit, &QLineEdit::textChanged, [this](const QString&) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        fileBox->addWidget(m_capWavFilePathEdit);

        auto browseBtn = new QPushButton("Open File...", w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path = QFileDialog::getOpenFileName(w, "Select WAV File", "", "WAV Files (*.wav);;All Files (*)");
            if (!path.isEmpty())
                m_capWavFilePathEdit->setText(path);
        });
        fileBox->addWidget(browseBtn);
        form->addRow("", fileBox);

        auto noteLbl = new QLabel("Sample rate, format, and channel count are parsed from the file header", w);
        noteLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        form->addRow("", noteLbl);

        auto wavExtrasDiv = new QFrame(w);
        wavExtrasDiv->setFrameShape(QFrame::HLine);
        wavExtrasDiv->setFrameShadow(QFrame::Sunken);
        wavExtrasDiv->setStyleSheet("background-color: #e5e5ea; max-height: 1px; margin-top: 4px; margin-bottom: 4px;");
        form->addRow("", wavExtrasDiv);

        m_capWavSkipBytesSpin = new QSpinBox(w);
        m_capWavSkipBytesSpin->setRange(0, 1000000);
        m_capWavSkipBytesSpin->setMinimumWidth(100);
        m_capWavSkipBytesSpin->setMaximumWidth(140);
        connect(m_capWavSkipBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Skip Bytes", m_capWavSkipBytesSpin);

        m_capWavReadBytesSpin = new QSpinBox(w);
        m_capWavReadBytesSpin->setRange(0, 100000000);
        m_capWavReadBytesSpin->setSpecialValueText("0 (All)");
        m_capWavReadBytesSpin->setMinimumWidth(100);
        m_capWavReadBytesSpin->setMaximumWidth(140);
        connect(m_capWavReadBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Read Bytes", m_capWavReadBytesSpin);

        m_capWavExtraSamplesSpin = new QSpinBox(w);
        m_capWavExtraSamplesSpin->setRange(0, 1000000);
        m_capWavExtraSamplesSpin->setMinimumWidth(120);
        m_capWavExtraSamplesSpin->setMaximumWidth(160);
        connect(m_capWavExtraSamplesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Extra Samples", m_capWavExtraSamplesSpin);

    } else {
        m_capRawFilePathEdit = new QLineEdit(w);
        m_capRawFilePathEdit->setPlaceholderText("e.g. /path/to/audio.raw");
        m_capRawFilePathEdit->setClearButtonEnabled(true);
        connect(m_capRawFilePathEdit, &QLineEdit::returnPressed, [this]() { applySettings(); });
        connect(m_capRawFilePathEdit, &QLineEdit::textChanged, [this](const QString&) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        fileBox->addWidget(m_capRawFilePathEdit);

        auto browseBtn = new QPushButton("Open File...", w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path =
                QFileDialog::getOpenFileName(w, "Select Raw File", "", "Raw Files (*.raw *.f64 *.f32);;All Files (*)");
            if (!path.isEmpty())
                m_capRawFilePathEdit->setText(path);
        });
        fileBox->addWidget(browseBtn);
        form->addRow("", fileBox);

        m_capRawFileFormatCombo = new QComboBox(w);
        m_capRawFileFormatCombo->addItems(
            {"S16_LE", "S24_3_LE", "S24_4_LJ_LE", "S24_4_RJ_LE", "S32_LE", "F32_LE", "F64_LE"});
        connect(m_capRawFileFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Format", m_capRawFileFormatCombo);

        m_capRawFileChannelsSpin = new QSpinBox(w);
        m_capRawFileChannelsSpin->setRange(1, 32);
        m_capRawFileChannelsSpin->setMinimumWidth(100);
        m_capRawFileChannelsSpin->setMaximumWidth(140);
        connect(m_capRawFileChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Channels", m_capRawFileChannelsSpin);

        auto rawExtrasDiv = new QFrame(w);
        rawExtrasDiv->setFrameShape(QFrame::HLine);
        rawExtrasDiv->setFrameShadow(QFrame::Sunken);
        rawExtrasDiv->setStyleSheet("background-color: #e5e5ea; max-height: 1px; margin-top: 4px; margin-bottom: 4px;");
        form->addRow("", rawExtrasDiv);

        m_capRawSkipBytesSpin = new QSpinBox(w);
        m_capRawSkipBytesSpin->setRange(0, 1000000);
        m_capRawSkipBytesSpin->setMinimumWidth(100);
        m_capRawSkipBytesSpin->setMaximumWidth(140);
        connect(m_capRawSkipBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Skip Bytes", m_capRawSkipBytesSpin);

        m_capRawReadBytesSpin = new QSpinBox(w);
        m_capRawReadBytesSpin->setRange(0, 100000000);
        m_capRawReadBytesSpin->setSpecialValueText("0 (All)");
        m_capRawReadBytesSpin->setMinimumWidth(100);
        m_capRawReadBytesSpin->setMaximumWidth(140);
        connect(m_capRawReadBytesSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Read Bytes", m_capRawReadBytesSpin);

        m_capRawExtraSamplesSpin = new QSpinBox(w);
        m_capRawExtraSamplesSpin->setRange(0, 1000000);
        m_capRawExtraSamplesSpin->setMinimumWidth(120);
        m_capRawExtraSamplesSpin->setMaximumWidth(160);
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

    auto genLbl = new QLabel("Generator", w);
    genLbl->setFixedWidth(100);

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
    form->addRow(genLbl, m_genTypeCombo);

    auto genChLbl = new QLabel("Channels", w);
    genChLbl->setFixedWidth(100);

    m_genChannelsSpin = new QSpinBox(w);
    m_genChannelsSpin->setRange(1, 32);
    m_genChannelsSpin->setMinimumWidth(100);
    m_genChannelsSpin->setMaximumWidth(140);
    connect(m_genChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
        if (m_isRefreshing)
            return;
        applySettings();
    });
    form->addRow(genChLbl, m_genChannelsSpin);

    auto freqBox = new QHBoxLayout();
    m_genFreqLabel = new QLabel("Freq (Hz)", w);
    m_genFreqLabel->setFixedWidth(100);

    m_genFreqSpin = new QDoubleSpinBox(w);
    m_genFreqSpin->setRange(1.0, 20000.0);
    m_genFreqSpin->setSingleStep(1.0);
    m_genFreqSpin->setSuffix(" Hz");
    m_genFreqSpin->setMinimumWidth(100);
    m_genFreqSpin->setMaximumWidth(140);

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
    m_genLevelSpin->setMinimumWidth(100);
    m_genLevelSpin->setMaximumWidth(140);

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

    m_pbDeviceListContainer = new QWidget(w);
    m_pbDeviceListLayout = new QVBoxLayout(m_pbDeviceListContainer);
    m_pbDeviceListLayout->setContentsMargins(0, 0, 0, 0);
    m_pbDeviceListLayout->setSpacing(2);

    auto devBox = new QVBoxLayout();
    devBox->addWidget(m_pbWarningWidget);
    devBox->addWidget(m_pbDeviceListContainer);
    form->addRow("", devBox);

    auto chBox = new QHBoxLayout();
    m_pbDevChannelsCombo = new QComboBox(w);
    m_pbDevChannelsSpin = new QSpinBox(w);
    m_pbDevChannelsSpin->setRange(1, 32);
    m_pbDevChannelsSpin->setMinimumWidth(110);
    m_pbDevChannelsSpin->setMaximumWidth(140);

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

    m_pbDevChannelsLabel = new QLabel("Device Channels", w);
    m_pbDevChannelsLabel->setFixedWidth(110);
    chBox->addWidget(m_pbDevChannelsLabel);
    chBox->addWidget(m_pbDevChannelsCombo);
    chBox->addWidget(m_pbDevChannelsSpin);

    m_pbStreamChannelsSpin = new QSpinBox(w);
    m_pbStreamChannelsSpin->setRange(1, 32);
    m_pbStreamChannelsSpin->setMinimumWidth(110);
    m_pbStreamChannelsSpin->setMaximumWidth(140);
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

    m_pbRateRow = new QWidget(w);
    auto rateBox = new QHBoxLayout(m_pbRateRow);
    rateBox->setContentsMargins(0, 0, 0, 0);
    auto rateLbl = new QLabel("Sample Rate", m_pbRateRow);
    rateLbl->setFixedWidth(100);
    rateBox->addWidget(rateLbl);

    m_pbRateCombo = new QComboBox(m_pbRateRow);
    connect(m_pbRateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() {
            applySettings();
            updateDoPCapability();
        });
    });
    rateBox->addWidget(m_pbRateCombo);
    rateBox->addStretch();
    form->addRow("", m_pbRateRow);

    m_pbFormatRow = new QWidget(w);
    auto fmtBox = new QHBoxLayout(m_pbFormatRow);
    fmtBox->setContentsMargins(0, 0, 0, 0);
    auto fmtLbl = new QLabel("Format", m_pbFormatRow);
    fmtLbl->setFixedWidth(100);
    fmtBox->addWidget(fmtLbl);

    m_pbFormatCombo = new QComboBox(m_pbFormatRow);
    connect(m_pbFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
        if (m_isRefreshing)
            return;
        QTimer::singleShot(0, [this]() { applySettings(); });
    });

    m_pbFormatLabel = new QLabel(m_pbFormatRow);
    m_pbFormatLabel->setStyleSheet("font-family: monospace; color: #8e8e93; font-size: 13px;");

    fmtBox->addWidget(m_pbFormatCombo);
    fmtBox->addWidget(m_pbFormatLabel);
    fmtBox->addStretch();
    form->addRow("", m_pbFormatRow);

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

    m_pbWasapiPollingCheck = new QCheckBox("WASAPI Polling Mode", w);
    connect(m_pbWasapiPollingCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    form->addRow("", m_pbWasapiPollingCheck);

    m_pbAlsaThreadedCheck = new QCheckBox("Threaded Ring Buffer Mode", w);
    connect(m_pbAlsaThreadedCheck, &QCheckBox::toggled, [this](bool) {
        if (!m_isRefreshing)
            applySettings();
    });
    form->addRow("", m_pbAlsaThreadedCheck);

    m_pbPipeWireRow = new QWidget(w);
    auto pbPwLayout = new QVBoxLayout(m_pbPipeWireRow);
    pbPwLayout->setContentsMargins(0, 0, 0, 0);
    pbPwLayout->setSpacing(8);

    auto pwPbNameBox = new QHBoxLayout();
    auto pwPbNameLbl = new QLabel("Node Name", m_pbPipeWireRow);
    pwPbNameLbl->setFixedWidth(130);
    pwPbNameBox->addWidget(pwPbNameLbl);
    m_pbPwNodeNameEdit = new QLineEdit(m_pbPipeWireRow);
    m_pbPwNodeNameEdit->setPlaceholderText("e.g. CamillaDSP");
    connect(m_pbPwNodeNameEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    pwPbNameBox->addWidget(m_pbPwNodeNameEdit);
    pbPwLayout->addLayout(pwPbNameBox);

    auto pwPbDescBox = new QHBoxLayout();
    auto pwPbDescLbl = new QLabel("Node Description", m_pbPipeWireRow);
    pwPbDescLbl->setFixedWidth(130);
    pwPbDescBox->addWidget(pwPbDescLbl);
    m_pbPwNodeDescEdit = new QLineEdit(m_pbPipeWireRow);
    m_pbPwNodeDescEdit->setPlaceholderText("e.g. CamillaDSP Playback");
    connect(m_pbPwNodeDescEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    pwPbDescBox->addWidget(m_pbPwNodeDescEdit);
    pbPwLayout->addLayout(pwPbDescBox);

    auto pwPbGroupBox = new QHBoxLayout();
    auto pwPbGroupLbl = new QLabel("Node Group", m_pbPipeWireRow);
    pwPbGroupLbl->setFixedWidth(130);
    pwPbGroupBox->addWidget(pwPbGroupLbl);
    m_pbPwNodeGroupEdit = new QLineEdit(m_pbPipeWireRow);
    connect(m_pbPwNodeGroupEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    pwPbGroupBox->addWidget(m_pbPwNodeGroupEdit);
    pbPwLayout->addLayout(pwPbGroupBox);

    auto pwPbAutoBox = new QHBoxLayout();
    auto pwPbAutoLbl = new QLabel("Autoconnect To", m_pbPipeWireRow);
    pwPbAutoLbl->setFixedWidth(130);
    pwPbAutoBox->addWidget(pwPbAutoLbl);
    m_pbPwAutoconnectEdit = new QLineEdit(m_pbPipeWireRow);
    connect(m_pbPwAutoconnectEdit, &QLineEdit::textChanged, [this](const QString&) {
        if (!m_isRefreshing)
            applySettings();
    });
    pwPbAutoBox->addWidget(m_pbPwAutoconnectEdit);
    pbPwLayout->addLayout(pwPbAutoBox);

    form->addRow("", m_pbPipeWireRow);

    m_pbDopDivider = new QFrame(w);
    m_pbDopDivider->setFrameShape(QFrame::HLine);
    m_pbDopDivider->setFrameShadow(QFrame::Sunken);
    m_pbDopDivider->setStyleSheet("background-color: #e5e5ea; max-height: 1px; margin-top: 4px; margin-bottom: 4px;");
    form->addRow("", m_pbDopDivider);

    m_outputDoPCheck = new QCheckBox("Output DoP (DSD-over-PCM)", w);
    connect(m_outputDoPCheck, &QCheckBox::toggled, [this](bool) {
        if (m_isRefreshing)
            return;
        applySettings();
        updateDoPCapability();
    });
    form->addRow("", m_outputDoPCheck);

    m_pbSdmFilterRow = new QWidget(w);
    auto sdmLayout = new QHBoxLayout(m_pbSdmFilterRow);
    sdmLayout->setContentsMargins(0, 0, 0, 0);
    m_sdmFilterLabel = new QLabel("SDM Filter", m_pbSdmFilterRow);
    m_sdmFilterLabel->setFixedWidth(100);
    sdmLayout->addWidget(m_sdmFilterLabel);

    m_sdmFilterCombo = new QComboBox(m_pbSdmFilterRow);
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
    sdmLayout->addWidget(m_sdmFilterCombo);
    sdmLayout->addStretch();
    form->addRow("", m_pbSdmFilterRow);

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
    bool isRust = m_devices && m_devices->isRustEngine();

    m_outputDoPCheck->setEnabled(isCapable && !isRust);
    bool sdmEnabled = isCapable && m_outputDoPCheck->isChecked() && !isRust;
    m_sdmFilterLabel->setEnabled(sdmEnabled);
    m_sdmFilterCombo->setEnabled(sdmEnabled);
    m_pbDopHintLabel->setVisible(!isCapable && !isRust);
}

QWidget* DevicePickerView::createPbFileView(bool isWav) {
    auto w = new QWidget();
    auto form = new QFormLayout(w);
    form->setSpacing(12);

    auto fileBox = new QHBoxLayout();
    auto pathLbl = new QLabel("File Path", w);
    pathLbl->setFixedWidth(100);
    fileBox->addWidget(pathLbl);

    if (isWav) {
        m_pbWavFilePathEdit = new QLineEdit(w);
        m_pbWavFilePathEdit->setPlaceholderText("e.g. /path/to/audio.wav");
        m_pbWavFilePathEdit->setClearButtonEnabled(true);
        connect(m_pbWavFilePathEdit, &QLineEdit::returnPressed, [this]() { applySettings(); });
        connect(m_pbWavFilePathEdit, &QLineEdit::textChanged, [this](const QString&) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        fileBox->addWidget(m_pbWavFilePathEdit);

        auto browseBtn = new QPushButton("Select File...", w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path =
                QFileDialog::getSaveFileName(w, "Select Output File", "", "WAV Files (*.wav);;All Files (*)");
            if (!path.isEmpty())
                m_pbWavFilePathEdit->setText(path);
        });
        fileBox->addWidget(browseBtn);
        form->addRow("", fileBox);

        m_pbWavFileFormatCombo = new QComboBox(w);
        m_pbWavFileFormatCombo->addItems({"S16_LE", "S24_3_LE", "S24_4_LJ_LE", "S32_LE", "F32_LE", "F64_LE"});
        connect(m_pbWavFileFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
            if (m_isRefreshing)
                return;
            QTimer::singleShot(0, [this]() { applySettings(); });
        });
        form->addRow("Format", m_pbWavFileFormatCombo);
        m_pbWavFileChannelsSpin = new QSpinBox(w);
        m_pbWavFileChannelsSpin->setRange(1, 32);
        m_pbWavFileChannelsSpin->setMinimumWidth(110);
        m_pbWavFileChannelsSpin->setMaximumWidth(140);
        connect(m_pbWavFileChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Channels", m_pbWavFileChannelsSpin);
        auto rf64Box = new QHBoxLayout();
        auto rf64Lbl = new QLabel("WAV Format", w);
        rf64Lbl->setFixedWidth(100);
        rf64Box->addWidget(rf64Lbl);

        m_pbWavUseRf64Combo = new QComboBox(w);
        m_pbWavUseRf64Combo->addItem("Standard (RIFF)", false);
        m_pbWavUseRf64Combo->addItem("RF64 (64-bit)", true);
        connect(m_pbWavUseRf64Combo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        rf64Box->addWidget(m_pbWavUseRf64Combo);
        rf64Box->addStretch();
        form->addRow("", rf64Box);
    } else {
        m_pbRawFilePathEdit = new QLineEdit(w);
        m_pbRawFilePathEdit->setPlaceholderText("e.g. /path/to/audio.raw");
        m_pbRawFilePathEdit->setClearButtonEnabled(true);
        connect(m_pbRawFilePathEdit, &QLineEdit::returnPressed, [this]() { applySettings(); });
        connect(m_pbRawFilePathEdit, &QLineEdit::textChanged, [this](const QString&) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        fileBox->addWidget(m_pbRawFilePathEdit);

        auto browseBtn = new QPushButton("Select File...", w);
        connect(browseBtn, &QPushButton::clicked, [this, w]() {
            QString path = QFileDialog::getSaveFileName(w, "Select Output File", "",
                                                        "Raw Files (*.raw *.f64 *.f32);;All Files (*)");
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
            QTimer::singleShot(0, [this]() { applySettings(); });
        });
        form->addRow("Format", m_pbRawFileFormatCombo);

        m_pbRawFileChannelsSpin = new QSpinBox(w);
        m_pbRawFileChannelsSpin->setRange(1, 32);
        m_pbRawFileChannelsSpin->setMinimumWidth(110);
        m_pbRawFileChannelsSpin->setMaximumWidth(140);
        connect(m_pbRawFileChannelsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this](int) {
            if (m_isRefreshing)
                return;
            applySettings();
        });
        form->addRow("Channels", m_pbRawFileChannelsSpin);
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
    if (m_devices->captureConfig.backend == AudioBackendType::PipeWire)
        isCapPw = true;
#endif

    // 1. Refresh Capture Devices List & CoreAudio controls
    if (!isCapPw) {
        populateDeviceList(m_capDeviceListLayout, m_capWarningWidget, m_capDeviceListContainer,
                           m_devices->captureDevices, m_devices->captureConfig.deviceName(), true);
    } else {
        m_capWarningWidget->hide();
        m_capDeviceListContainer->hide();
    }

    int capBackendIdx = m_capBackendCombo->findData(static_cast<int>(m_devices->captureConfig.backend));
    if (capBackendIdx >= 0) {
        m_capBackendCombo->blockSignals(true);
        m_capBackendCombo->setCurrentIndex(capBackendIdx);
        m_capBackendCombo->blockSignals(false);
    }
    m_capStack->setCurrentIndex(getCapStackIndex(m_devices->captureConfig.backend));

    // Capture Channels (Device Channels combo vs spinbox)
    m_capDevChannelsLabel->setVisible(!isCapPw);
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
    m_capFormatRow->setVisible(!isCapPw);
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
    }

    // Capture DoP
    bool capDopVisible = !m_devices->isRustEngine() && !isCapPw && isHardwareBackend(m_devices->captureConfig.backend);
    m_capDopDivider->setVisible(capDopVisible);
    m_bypassDoPCheck->setVisible(capDopVisible);
    m_capDopCutoffRow->setVisible(capDopVisible);
    m_dopCutoffHint->setVisible(capDopVisible);

    m_bypassDoPCheck->setChecked(m_devices->captureConfig.bypassDoP);
    m_dopCutoffLabel->setEnabled(!m_devices->captureConfig.bypassDoP);
    m_dopCutoffCombo->setEnabled(!m_devices->captureConfig.bypassDoP);
    int cutoffIdx = m_dopCutoffCombo->findData(m_devices->captureConfig.dopCutoffHz);
    if (cutoffIdx >= 0)
        m_dopCutoffCombo->setCurrentIndex(cutoffIdx);

    bool isCapWasapi = false;
#if defined(ENABLE_WASAPI)
    isCapWasapi = (m_devices->captureConfig.backend == AudioBackendType::WASAPI);
#endif
    bool isCapAlsa = false;
#if defined(ENABLE_ALSA)
    if (m_devices->captureConfig.backend == AudioBackendType::ALSA)
        isCapAlsa = true;
#endif
    m_capWasapiExclusiveCheck->setChecked(m_devices->captureConfig.exclusive);
    m_capWasapiExclusiveCheck->setVisible(isCapWasapi);
    m_capWasapiLoopbackCheck->setChecked(m_devices->captureConfig.loopback);
    m_capWasapiLoopbackCheck->setVisible(isCapWasapi);
    m_capWasapiPollingCheck->setChecked(m_devices->captureConfig.polling);
    m_capWasapiPollingCheck->setVisible(isCapWasapi);
    m_capAlsaStopInactiveCheck->setChecked(m_devices->captureConfig.stopOnInactive);
    m_capAlsaStopInactiveCheck->setVisible(isCapAlsa);
    m_capAlsaThreadedCheck->setChecked(m_devices->captureConfig.threaded);
    m_capAlsaThreadedCheck->setVisible(isCapAlsa);
    m_capAlsaLinkVolRow->setVisible(isCapAlsa);
    m_capAlsaLinkMuteRow->setVisible(isCapAlsa);
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

    m_capPipeWireRow->setVisible(isCapPw);
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
    m_genFreqLabel->setEnabled(!isNoise);
    m_genFreqSpin->setEnabled(!isNoise);
    m_genFreqSlider->setEnabled(!isNoise);

    // 3. Refresh Playback Devices List & CoreAudio controls
    bool isPbPw = false;
#if defined(ENABLE_PIPEWIRE)
    if (m_devices->playbackConfig.backend == AudioBackendType::PipeWire)
        isPbPw = true;
#endif

    if (!isPbPw) {
        populateDeviceList(m_pbDeviceListLayout, m_pbWarningWidget, m_pbDeviceListContainer, m_devices->playbackDevices,
                           m_devices->playbackConfig.deviceName(), false);
    } else {
        m_pbWarningWidget->hide();
        m_pbDeviceListContainer->hide();
    }

    int pbBackendIdx = m_pbBackendCombo->findData(static_cast<int>(m_devices->playbackConfig.backend));
    if (pbBackendIdx >= 0) {
        m_pbBackendCombo->blockSignals(true);
        m_pbBackendCombo->setCurrentIndex(pbBackendIdx);
        m_pbBackendCombo->blockSignals(false);
    }
    m_pbStack->setCurrentIndex(getPbStackIndex(m_devices->playbackConfig.backend));

    // Playback Channels (Device Channels combo vs spinbox)
    m_pbDevChannelsLabel->setVisible(!isPbPw);
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
    m_pbRateRow->show();
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
    m_pbFormatRow->setVisible(!isPbPw);
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
    }

    bool isPbWasapi = false;
#if defined(ENABLE_WASAPI)
    isPbWasapi = (m_devices->playbackConfig.backend == AudioBackendType::WASAPI);
#endif
    bool isPbCoreAudio = false;
#if defined(ENABLE_COREAUDIO)
    isPbCoreAudio = (m_devices->playbackConfig.backend == AudioBackendType::CoreAudio);
#endif

    bool pbExclusiveVisible = isPbWasapi || isPbCoreAudio;
    m_exclusiveModeCheck->setVisible(pbExclusiveVisible);
    m_exclusiveModeHint->setVisible(pbExclusiveVisible);
    m_exclusiveModeCheck->setChecked(m_devices->playbackConfig.exclusive);

    m_pbWasapiPollingCheck->setVisible(isPbWasapi);
    m_pbWasapiPollingCheck->setChecked(m_devices->playbackConfig.polling);

    bool isPbAlsa = false;
#if defined(ENABLE_ALSA)
    if (m_devices->playbackConfig.backend == AudioBackendType::ALSA)
        isPbAlsa = true;
#endif
    m_pbAlsaThreadedCheck->setVisible(isPbAlsa);
    m_pbAlsaThreadedCheck->setChecked(m_devices->playbackConfig.threaded);

    m_pbPipeWireRow->setVisible(isPbPw);
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

    bool pbDopVisible = !m_devices->isRustEngine() && !isPbPw && isHardwareBackend(m_devices->playbackConfig.backend);
    m_pbDopDivider->setVisible(pbDopVisible);
    m_outputDoPCheck->setVisible(pbDopVisible);
    m_pbSdmFilterRow->setVisible(pbDopVisible);

    m_outputDoPCheck->setChecked(m_devices->playbackConfig.outputDoP);

    int filterIdx = m_sdmFilterCombo->findData(static_cast<int>(m_devices->playbackConfig.dsdEncoderFilter));
    if (filterIdx >= 0)
        m_sdmFilterCombo->setCurrentIndex(filterIdx);

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
            pbCfg.format = DeviceConfig::defaultFormatForBackend(AudioBackendType::PipeWire);
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
            if (!pbFormats.empty() && m_pbFormatCombo->isVisible()) {
                pbCfg.format = m_pbFormatCombo->currentText().toStdString();
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
            capCfg.format = DeviceConfig::defaultFormatForBackend(AudioBackendType::PipeWire);
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
            if (!capFormats.empty() && m_capFormatCombo->isVisible()) {
                capCfg.format = m_capFormatCombo->currentText().toStdString();
            }
        }

        capCfg.bypassDoP = m_bypassDoPCheck->isChecked();
        if (m_dopCutoffCombo->currentIndex() >= 0) {
            capCfg.dopCutoffHz = m_dopCutoffCombo->currentData().toDouble();
        }
        capCfg.exclusive = m_capWasapiExclusiveCheck->isChecked();
        capCfg.loopback = m_capWasapiLoopbackCheck->isChecked();
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
