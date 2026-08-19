#include "ui/PipelineOverviewWidget.h"

#include <QAction>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

OverviewCanvasWidget::OverviewCanvasWidget(PipelineOverviewWidget* owner, QWidget* parent)
    : QWidget(parent), m_owner(owner) {}

void OverviewCanvasWidget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
}

PipelineOverviewWidget::PipelineOverviewWidget(std::shared_ptr<DSPEngineController> dspController, QWidget* parent)
    : QGroupBox("Signal Chain", parent), m_dspController(dspController) {
    setupUi();

    if (m_dspController) {
        if (m_dspController->settings()) {
            connect(m_dspController->settings().get(), &AudioSettings::settingsChanged, this,
                    &PipelineOverviewWidget::rebuildOverview);
        }
        if (m_dspController->pipelineStore()) {
            connect(m_dspController->pipelineStore().get(), &PipelineStore::pipelineChanged, this,
                    &PipelineOverviewWidget::rebuildOverview);
        }
        connect(m_dspController.get(), &DSPEngineController::statusChanged, this,
                &PipelineOverviewWidget::rebuildOverview);
    }

    rebuildOverview();
}

void PipelineOverviewWidget::setupUi() {
    auto rootLayout = new QVBoxLayout(this);

    // 1. Toolbar Controls Row
    auto toolbarLayout = new QHBoxLayout();

    m_statsLabel = new QLabel(this);
    toolbarLayout->addWidget(m_statsLabel);

    m_warningBadge = new QLabel(tr("Broken Chain"), this);
    m_warningBadge->setVisible(false);
    toolbarLayout->addWidget(m_warningBadge);

    toolbarLayout->addStretch();

    // Categorized Add Stage Menu Button
    m_addStageBtn = new QPushButton(tr("Add Stage…"), this);
    buildAddStageMenu();
    toolbarLayout->addWidget(m_addStageBtn);

    // Details Toggle Button
    m_toggleDetailsBtn = new QPushButton(tr("Hide Elementary Steps"), this);
    connect(m_toggleDetailsBtn, &QPushButton::clicked, [this]() {
        m_showElementaryDetails = !m_showElementaryDetails;
        m_toggleDetailsBtn->setText(m_showElementaryDetails ? tr("Hide Elementary Steps")
                                                            : tr("Show Elementary Steps"));
        rebuildOverview();
    });
    toolbarLayout->addWidget(m_toggleDetailsBtn);

    rootLayout->addLayout(toolbarLayout);

    // 2. Channel Signal Wires Legend Bar
    auto legendRow = new QHBoxLayout();

    auto legendTitle = new QLabel(tr("Channel Wires:"), this);
    legendRow->addWidget(legendTitle);

    auto legendScroll = new QScrollArea(this);
    legendScroll->setWidgetResizable(true);
    legendScroll->setFrameShape(QFrame::NoFrame);
    legendScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    legendScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    legendScroll->setFixedHeight(32);
    legendScroll->setStyleSheet("QScrollArea { background: transparent; border: none; }\n"
                                "QScrollArea > QWidget > QWidget { background: transparent; border: none; }\n"
                                "QWidget#LegendViewport { background: transparent; border: none; }\n"
                                "QWidget#LegendContainer { background: transparent; border: none; }");
    legendScroll->viewport()->setObjectName("LegendViewport");
    legendScroll->viewport()->setAutoFillBackground(false);
    legendScroll->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);

    m_legendContainerWidget = new QWidget(legendScroll);
    m_legendContainerWidget->setObjectName("LegendContainer");
    m_legendContainerWidget->setAutoFillBackground(false);
    m_legendContainerWidget->setAttribute(Qt::WA_TranslucentBackground, true);
    m_legendBarLayout = new QHBoxLayout(m_legendContainerWidget);
    m_legendBarLayout->setContentsMargins(0, 0, 0, 0);
    m_legendBarLayout->setSpacing(6);
    m_legendBarLayout->setAlignment(Qt::AlignLeft);
    legendScroll->setWidget(m_legendContainerWidget);
    legendRow->addWidget(legendScroll, 1);

    rootLayout->addLayout(legendRow);

    // 3. Scrollable Canvas
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_canvasWidget = new OverviewCanvasWidget(this, m_scrollArea);
    m_canvasLayout = new QHBoxLayout(m_canvasWidget);
    m_canvasLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_scrollArea->setWidget(m_canvasWidget);
    rootLayout->addWidget(m_scrollArea);
}

void PipelineOverviewWidget::buildAddStageMenu() {
    if (!m_addStageBtn)
        return;

    auto menu = new QMenu(m_addStageBtn);

    auto addCategorySubMenu = [this, menu](const QString& categoryName, const std::vector<StageType>& types) {
        auto subMenu = menu->addMenu(categoryName);
        for (StageType type : types) {
            QString name = QString::fromStdString(stageTypeToString(type));
            auto action = subMenu->addAction(name);
            connect(action, &QAction::triggered, [this, type]() {
                if (m_dspController && m_dspController->pipelineStore()) {
                    m_dspController->pipelineStore()->addStage(type);
                    m_dspController->applyConfig();
                    rebuildOverview();
                }
            });
        }
    };

    addCategorySubMenu("Filters",
                       {StageType::EQ, StageType::GraphicEQ, StageType::Convolution, StageType::BiquadCombo,
                        StageType::DiffEq, StageType::Gain, StageType::Delay, StageType::Volume, StageType::Clipper,
                        StageType::LookaheadLimiter, StageType::Dither, StageType::Loudness});

    addCategorySubMenu("Mixer", {StageType::MatrixMixer});

    addCategorySubMenu("Processors",
                       {StageType::Compressor, StageType::NoiseGate, StageType::RACE, StageType::LookaheadLimiterProc});

    addCategorySubMenu("Others",
                       {StageType::Balance, StageType::Width, StageType::MSProc, StageType::PhaseInvert,
                        StageType::Crossfeed, StageType::DCProtection, StageType::Emphasis, StageType::SplitWidth});

    m_addStageBtn->setMenu(menu);
}

QIcon PipelineOverviewWidget::createChannelDotIcon(int ch, bool /*hovered*/) const {
    QColor col = channelColor(ch);
    QPixmap px(8, 8);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(col);
    p.setPen(Qt::NoPen);
    p.drawEllipse(0, 0, 8, 8);
    p.end();
    return QIcon(px);
}

void PipelineOverviewWidget::rebuildLegendBar(int maxChannels) {
    if (!m_legendBarLayout)
        return;

    m_legendBtnMap.clear();

    QLayoutItem* item;
    while ((item = m_legendBarLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            delete item->widget();
        delete item;
    }

    for (int ch = 0; ch < maxChannels; ++ch) {
        auto pillBtn = new QPushButton(QString("Ch %1").arg(ch + 1), m_legendContainerWidget);
        pillBtn->installEventFilter(this);
        updateLegendPillStyle(pillBtn, ch, false);
        m_legendBarLayout->addWidget(pillBtn);
        m_legendBtnMap[pillBtn] = ch;
    }
    m_legendBarLayout->addStretch();
}

void PipelineOverviewWidget::updateLegendPillStyle(QObject* obj, int ch, bool hovered) {
    auto btn = qobject_cast<QPushButton*>(obj);
    if (!btn)
        return;

    btn->setIcon(createChannelDotIcon(ch, hovered));
    btn->setIconSize(QSize(8, 8));
}

bool PipelineOverviewWidget::eventFilter(QObject* watched, QEvent* event) {
    auto it = m_legendBtnMap.find(watched);
    if (it != m_legendBtnMap.end()) {
        int ch = it->second;
        if (event->type() == QEvent::Enter) {
            m_hoveredChannel = ch;
            updateLegendPillStyle(watched, ch, true);
            if (m_canvasWidget)
                m_canvasWidget->update();
            return true;
        } else if (event->type() == QEvent::Leave) {
            m_hoveredChannel = std::nullopt;
            updateLegendPillStyle(watched, ch, false);
            if (m_canvasWidget)
                m_canvasWidget->update();
            return true;
        }
    }
    return QGroupBox::eventFilter(watched, event);
}

QColor PipelineOverviewWidget::channelColor(int index) const {
    switch (index) {
    case 0:
        return QColor(0, 122, 255); // Blue (#007aff)
    case 1:
        return QColor(175, 82, 222); // Purple (#af52de)
    case 2:
        return QColor(255, 149, 0); // Orange (#ff9500)
    case 3:
        return QColor(52, 199, 89); // Green (#34c759)
    case 4:
        return QColor(50, 173, 230); // Cyan (#32ade6)
    default:
        return QColor(255, 45, 85); // Pink (#ff2d55)
    }
}

QString PipelineOverviewWidget::categoryColorHex(StageCategory cat) const {
    switch (cat) {
    case StageCategory::Filters:
        return "0, 122, 255";
    case StageCategory::Mixer:
        return "175, 82, 222";
    case StageCategory::Processors:
        return "255, 149, 0";
    case StageCategory::Others:
        return "90, 200, 250";
    }
    return "0, 122, 255";
}

QString PipelineOverviewWidget::stepTypeColorHex(PipelineStepType type) const {
    switch (type) {
    case PipelineStepType::Filter:
        return "0, 122, 255"; // Blue
    case PipelineStepType::Mixer:
        return "175, 82, 222"; // Purple
    case PipelineStepType::Processor:
        return "255, 149, 0"; // Orange
    }
    return "0, 122, 255";
}

QString PipelineOverviewWidget::stepTypeTitle(PipelineStepType type) const {
    switch (type) {
    case PipelineStepType::Filter:
        return "Filter Chain";
    case PipelineStepType::Mixer:
        return "Matrix / Routing Mixer";
    case PipelineStepType::Processor:
        return "Dynamics Processor";
    }
    return "Filter Chain";
}

void PipelineOverviewWidget::paintCanvasWires(QWidget* canvasWidget) {
    if (!canvasWidget)
        return;
    QPainter painter(canvasWidget);
    painter.setRenderHint(QPainter::Antialiasing);

    int maxCh = std::max(1, m_lastMaxChannels);
    double topY = 40.0;
    double trackSpacing = 28.0;

    for (int ch = 0; ch < maxCh; ++ch) {
        bool isHighlighted = (!m_hoveredChannel.has_value() || m_hoveredChannel.value() == ch);
        QColor col = channelColor(ch);
        QPen pen(col, isHighlighted ? 2.5 : 1.0, Qt::SolidLine);
        painter.setPen(pen);

        double y = topY + ch * trackSpacing;
        painter.drawLine(QPointF(10.0, y), QPointF(canvasWidget->width() - 10.0, y));
    }
}

QString PipelineOverviewWidget::formatSampleRate(int rate) const {
    if (rate <= 0)
        return "44.1 kHz";
    if (rate % 1000 == 0) {
        return QString("%1 kHz").arg(rate / 1000);
    } else {
        return QString::asprintf("%.1f kHz", rate / 1000.0);
    }
}

QString PipelineOverviewWidget::readableFilterName(const std::string& rawName, const PipelineStage& stage) const {
    QString qRaw = QString::fromStdString(rawName);
    QStringList parts = qRaw.split('_');
    QString suffix = parts.last();

    if (suffix == "preamp") {
        float gain = 0.0f;
        if (m_dspController && m_dspController->pipelineStore() && stage.eqPresetId.has_value()) {
            for (const auto& p : m_dspController->pipelineStore()->eqPresets) {
                if (p.id == stage.eqPresetId.value()) {
                    gain = p.preampGain;
                    break;
                }
            }
        }
        return QString("Preamp (%1 dB)").arg(QString::asprintf("%+.1f", gain));
    }
    if (suffix == "invert")
        return "Phase Invert (180°)";
    if (suffix == "hi")
        return "Highshelf Filter";
    if (suffix == "lo")
        return "Lowpass Filter";
    if (suffix == "lo_gain")
        return "Crossfeed Low Attenuation";
    if (suffix == "lp")
        return "Linkwitz-Riley Lowpass (12dB/oct)";
    if (suffix == "hp")
        return "Linkwitz-Riley Highpass (12dB/oct)";
    if (suffix == "conv")
        return "Convolution IR Engine";
    if (suffix == "loudness")
        return "Fader Loudness Compensation";
    if (suffix == "deemphasis")
        return "CD De-emphasis Filter";
    if (suffix == "preemphasis")
        return "Pre-emphasis Boost";
    if (suffix == "dcp")
        return "DC Protection Highpass (7Hz)";
    if (suffix == "gain")
        return QString("Gain (%1 dB)").arg(QString::asprintf("%+.1f", stage.gainValue));
    if (suffix == "delay")
        return QString("Delay (%1 ms)").arg(QString::asprintf("%.1f", stage.delayValue));
    if (suffix == "volume")
        return QString("Volume (%1)").arg(QString::fromStdString(faderToString(stage.volumeFader)));
    if (suffix == "lookahead_limiter")
        return "Lookahead Limiter";
    if (suffix == "dither")
        return QString("Dither (%1-bit)").arg(stage.ditherBits);
    if (suffix == "diffeq")
        return "DiffEq IIR/FIR";
    if (suffix == "combo")
        return QString("Biquad Combo (%1)").arg(QString::fromStdString(biquadComboTypeToString(stage.comboType)));
    if (suffix == "clipper")
        return QString("Clipper (%1 dB)").arg(QString::asprintf("%+.1f", stage.clipperLimit));
    if (suffix == "geq")
        return QString("Graphic EQ (%1 Bands)").arg(stage.graphicEQBandCount);

    bool isNum = false;
    int bandNum = suffix.toInt(&isNum);
    if (isNum) {
        return QString("Biquad Band #%1").arg(bandNum);
    }
    return suffix;
}

QString PipelineOverviewWidget::readableMixerOrProcessorName(const std::string& rawName,
                                                             const PipelineStage& stage) const {
    if (rawName.find("2to4") != std::string::npos) {
        return "Expand 2ch ➔ 4ch (Stereo Split)";
    }
    if (rawName.find("4to2") != std::string::npos) {
        return "Sum 4ch ➔ 2ch (Stereo Blend)";
    }

    switch (stage.type) {
    case StageType::Balance:
        return QString("Balance Mapping (Pos: %1%)").arg(static_cast<int>(stage.balancePosition * 100));
    case StageType::Width:
        return QString("Stereo Width Matrix (%1%)").arg(stage.widthPercent());
    case StageType::MSProc:
        return "Mid/Side Encoding/Decoding Matrix";
    case StageType::MatrixMixer:
        return QString("Matrix Mixer (%1 ➔ %2 ch)").arg(stage.mixerChannelsIn).arg(stage.mixerChannelsOut);
    case StageType::Compressor:
        return QString("Compressor (Ratio: %1:1, Thresh: %2dB)")
            .arg(QString::asprintf("%.1f", stage.compressorRatio))
            .arg(static_cast<int>(stage.compressorThreshold));
    case StageType::NoiseGate:
        return QString("Noise Gate (Thresh: %1dB)").arg(static_cast<int>(stage.gateThreshold));
    case StageType::RACE:
        return QString("RACE Spatialization (%1ms)").arg(stage.raceDelay);
    default:
        return QString::fromStdString(rawName);
    }
}

void PipelineOverviewWidget::rebuildOverview() {
    if (!m_canvasLayout || !m_canvasWidget)
        return;

    // Clear existing widgets
    QLayoutItem* item;
    while ((item = m_canvasLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            delete item->widget();
        delete item;
    }

    auto devMgr = m_dspController ? m_dspController->devices() : nullptr;
    auto settings = m_dspController ? m_dspController->settings() : nullptr;
    auto pipe = m_dspController ? m_dspController->pipelineStore() : nullptr;
    bool isRunning = (m_dspController && m_dspController->status == ProcessingState::Running);

    int captureCh = devMgr ? std::max(1, devMgr->captureConfig.channels) : 2;
    int captureRate = devMgr ? devMgr->captureConfig.sampleRate : 48000;
    int playbackCh = devMgr ? std::max(1, devMgr->playbackConfig.channels) : 2;
    int playbackRate = devMgr ? devMgr->playbackConfig.sampleRate : 48000;

    std::string capDevName = devMgr ? devMgr->captureConfig.deviceName().value_or("CoreAudio Input") : "Capture Input";
    std::string playDevName =
        devMgr ? devMgr->playbackConfig.deviceName().value_or("CoreAudio Output") : "Playback Output";

    int finalOutputCh = captureCh;
    if (pipe && !pipe->stages.empty()) {
        finalOutputCh = pipe->channelCountBeforeStage(pipe->stages.size(), captureCh);
    }
    bool isOutputMismatch = (finalOutputCh != playbackCh);

    m_warningBadge->setVisible(isOutputMismatch);
    if (isOutputMismatch) {
        m_warningBadge->setText(
            QString("Broken Chain: Outputs %1 ch, Device expects %2 ch").arg(finalOutputCh).arg(playbackCh));
    }

    int activeStageCount = 0;
    int totalElementarySteps = 0;
    int maxCh = std::max(captureCh, playbackCh);

    if (pipe) {
        std::map<QUuid, EQPreset> eqMap;
        for (const auto& p : pipe->eqPresets)
            eqMap[p.id] = p;
        std::map<QUuid, ConvolutionPreset> convMap;
        for (const auto& p : pipe->convPresets)
            convMap[p.id] = p;

        for (size_t i = 0; i < pipe->stages.size(); ++i) {
            const auto& st = pipe->stages[i];
            if (st.isEnabled)
                activeStageCount++;

            int inCh = pipe->channelCountBeforeStage(i, captureCh);
            int outCh = (st.isActive() && st.type == StageType::MatrixMixer) ? st.mixerChannelsOut : inCh;
            maxCh = std::max({maxCh, inCh, outCh});

            auto res = StageBuilders::buildStage(st, captureRate, inCh, eqMap, convMap);
            totalElementarySteps += res.steps.size();
        }
    }

    m_lastMaxChannels = std::max(2, maxCh);
    rebuildLegendBar(m_lastMaxChannels);

    m_statsLabel->setText(QString("%1 Ch In (%2) • %3 Active Stages • %4 Elementary Steps • %5 Ch Out (%6)")
                              .arg(captureCh)
                              .arg(formatSampleRate(captureRate))
                              .arg(activeStageCount)
                              .arg(totalElementarySteps)
                              .arg(playbackCh)
                              .arg(formatSampleRate(playbackRate)));

    auto addConnector = [this](int fromCh, int toCh, bool isMismatch = false) {
        auto connWidget = new QWidget(m_canvasWidget);
        auto connLayout = new QVBoxLayout(connWidget);
        connLayout->setContentsMargins(0, 0, 0, 0);

        auto arrowLabel = new QLabel(isMismatch ? "!" : "➔", connWidget);
        arrowLabel->setAlignment(Qt::AlignCenter);
        connLayout->addWidget(arrowLabel);

        QString labelText;
        if (isMismatch) {
            labelText = QString("%1 ch (Mismatch)").arg(fromCh);
        } else if (fromCh != toCh) {
            labelText = QString("%1➔%2 ch").arg(fromCh).arg(toCh);
        } else {
            labelText = QString("%1 ch").arg(fromCh);
        }

        auto tagLabel = new QLabel(labelText, connWidget);
        tagLabel->setAlignment(Qt::AlignCenter);
        connLayout->addWidget(tagLabel);

        m_canvasLayout->addWidget(connWidget, 0, Qt::AlignVCenter);
    };

    // Helper: Build Graph Node Card Box
    auto createNodeCard = [this](const QString& title, const QString& subtitle, const QString& iconStr = "") {
        auto card = new QFrame(m_canvasWidget);
        card->setFrameShape(QFrame::StyledPanel);
        card->setFixedWidth(220);
        card->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);

        auto cardLayout = new QVBoxLayout(card);

        // Header Row: Icon + Title & Subtitle
        auto headerRow = new QHBoxLayout();
        if (!iconStr.isEmpty()) {
            auto iconDot = new QLabel(iconStr, card);
            iconDot->setAlignment(Qt::AlignCenter);
            headerRow->addWidget(iconDot);
        }

        auto titleVBox = new QVBoxLayout();
        auto titleLbl = new QLabel(title, card);
        QFont titleFont = titleLbl->font();
        titleFont.setBold(true);
        titleLbl->setFont(titleFont);
        titleVBox->addWidget(titleLbl);

        if (!subtitle.isEmpty()) {
            auto subLbl = new QLabel(subtitle, card);
            titleVBox->addWidget(subLbl);
        }
        headerRow->addLayout(titleVBox, 1);
        cardLayout->addLayout(headerRow);

        return std::make_pair(card, cardLayout);
    };

    auto createScrollableChannelPills = [](int channelCount, QWidget* parent) -> QWidget* {
        auto container = new QWidget(parent);
        auto layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        for (int c = 0; c < channelCount; ++c) {
            auto pill = new QLabel(QString::number(c + 1), container);
            pill->setAlignment(Qt::AlignCenter);
            layout->addWidget(pill);
        }
        layout->addStretch();
        return container;
    };

    // 1. Capture Input Node
    auto [capCard, capLayout] = createNodeCard(tr("Input (Capture)"), QString::fromStdString(capDevName));

    auto capForm = new QFormLayout();
    capForm->addRow(tr("Channels:"), new QLabel(QString::number(captureCh), capCard));
    capForm->addRow(tr("Sample Rate:"), new QLabel(formatSampleRate(captureRate), capCard));
    capLayout->addLayout(capForm);
    capLayout->addWidget(createScrollableChannelPills(captureCh, capCard));

    m_canvasLayout->addWidget(capCard, 0, Qt::AlignVCenter);

    // Connector 1
    addConnector(captureCh, captureCh);

    // 2. Resampler Node (if active)
    bool resampEnabled = settings ? settings->resamplerEnabled : false;
    if (resampEnabled) {
        auto [resampCard, resampLayout] = createNodeCard(tr("Resampler"), tr("Synchronous SRC"));

        auto resampForm = new QFormLayout();
        resampForm->addRow(tr("Channels:"), new QLabel(QString("%1 ch").arg(captureCh), resampCard));
        resampForm->addRow(tr("Target Rate:"), new QLabel(formatSampleRate(captureRate), resampCard));
        resampLayout->addLayout(resampForm);

        m_canvasLayout->addWidget(resampCard, 0, Qt::AlignVCenter);
        addConnector(captureCh, captureCh);
    }

    // 3. Pipeline Stage Nodes
    if (pipe) {
        std::map<QUuid, EQPreset> eqMap;
        for (const auto& p : pipe->eqPresets)
            eqMap[p.id] = p;
        std::map<QUuid, ConvolutionPreset> convMap;
        for (const auto& p : pipe->convPresets)
            convMap[p.id] = p;

        for (size_t i = 0; i < pipe->stages.size(); ++i) {
            auto& stage = pipe->stages[i];
            int inCh = pipe->channelCountBeforeStage(i, captureCh);
            int outCh = (stage.isActive() && stage.type == StageType::MatrixMixer) ? stage.mixerChannelsOut : inCh;
            int nextInCh = (i + 1 < pipe->stages.size()) ? pipe->channelCountBeforeStage(i + 1, captureCh) : playbackCh;

            bool isMismatched = (outCh != nextInCh);
            bool active = stage.isEnabled && stage.isActive();

            auto [stCard, stLayout] = createNodeCard(QString::fromStdString(stage.name),
                                                     QString::fromStdString(stageTypeToString(stage.type)));

            // Add Stage Bypass Button in header row
            auto headerLayout = qobject_cast<QHBoxLayout*>(stLayout->itemAt(0)->layout());
            if (headerLayout) {
                auto bypassDotBtn = new QPushButton(active ? tr("Enabled") : tr("Bypassed"), stCard);
                bypassDotBtn->setCheckable(true);
                bypassDotBtn->setChecked(active);
                bypassDotBtn->setToolTip(active ? tr("Click to disable stage") : tr("Click to enable stage"));
                connect(bypassDotBtn, &QPushButton::clicked, [this, i]() {
                    if (m_dspController && m_dspController->pipelineStore()) {
                        m_dspController->pipelineStore()->stages[i].isEnabled =
                            !m_dspController->pipelineStore()->stages[i].isEnabled;
                        m_dspController->applyConfig();
                        rebuildOverview();
                    }
                });
                headerLayout->addWidget(bypassDotBtn, 0, Qt::AlignVCenter);
            }

            // Channel routing row with QFormLayout
            auto stageForm = new QFormLayout();
            QString routeText =
                (inCh != outCh) ? QString("%1 In ➔ %2 Out").arg(inCh).arg(outCh) : QString("%1 Ch").arg(inCh);
            stageForm->addRow(tr("Routing:"), new QLabel(routeText, stCard));
            stLayout->addLayout(stageForm);

            // Elementary Steps breakdown
            if (m_showElementaryDetails && active) {
                auto stepsRes = StageBuilders::buildStage(stage, captureRate, inCh, eqMap, convMap);

                for (const auto& step : stepsRes.steps) {
                    auto stepWidget = new QFrame(stCard);
                    stepWidget->setFrameShape(QFrame::StyledPanel);
                    auto stepVBox = new QVBoxLayout(stepWidget);

                    auto stepHeader = new QHBoxLayout();

                    QString stepTitle = stepTypeTitle(step.type);
                    auto titleLbl = new QLabel(stepTitle, stepWidget);
                    QFont font = titleLbl->font();
                    font.setBold(true);
                    titleLbl->setFont(font);
                    stepHeader->addWidget(titleLbl, 1);

                    // Target channels tag
                    std::vector<int> chList;
                    if (!step.channels.empty()) {
                        chList = step.channels;
                    } else if (step.channel.has_value()) {
                        chList = {step.channel.value()};
                    } else if (step.type == PipelineStepType::Processor) {
                        for (int c : stage.channels) {
                            if (c < inCh)
                                chList.push_back(c);
                        }
                        std::sort(chList.begin(), chList.end());
                    } else {
                        for (int c = 0; c < inCh; ++c)
                            chList.push_back(c);
                    }

                    auto chTagsBox = new QHBoxLayout();
                    if (chList.size() == static_cast<size_t>(inCh) && inCh > 4) {
                        auto chTag = new QLabel(tr("All"), stepWidget);
                        chTag->setAlignment(Qt::AlignCenter);
                        chTagsBox->addWidget(chTag);
                    } else if (chList.size() > 4) {
                        bool isContiguous = true;
                        for (size_t k = 1; k < chList.size(); ++k) {
                            if (chList[k] != chList[k - 1] + 1) {
                                isContiguous = false;
                                break;
                            }
                        }
                        QString labelText;
                        if (isContiguous) {
                            labelText = QString("%1–%2").arg(chList.front() + 1).arg(chList.back() + 1);
                        } else {
                            labelText = QString("%1 ch").arg(chList.size());
                        }
                        auto chTag = new QLabel(labelText, stepWidget);
                        chTag->setAlignment(Qt::AlignCenter);
                        chTagsBox->addWidget(chTag);
                    } else {
                        for (int ch : chList) {
                            auto chTag = new QLabel(QString::number(ch + 1), stepWidget);
                            chTag->setAlignment(Qt::AlignCenter);
                            chTagsBox->addWidget(chTag);
                        }
                    }
                    stepHeader->addLayout(chTagsBox);
                    stepVBox->addLayout(stepHeader);

                    // Unrolled Step filter / mixer names
                    if (!step.names.empty()) {
                        for (const auto& rawN : step.names) {
                            auto itemHBox = new QHBoxLayout();
                            auto dotLbl = new QLabel("•", stepWidget);
                            itemHBox->addWidget(dotLbl);

                            auto nLbl = new QLabel(readableFilterName(rawN, stage), stepWidget);
                            itemHBox->addWidget(nLbl, 1);

                            stepVBox->addLayout(itemHBox);
                        }
                    } else if (step.name.has_value()) {
                        auto nLbl = new QLabel(readableMixerOrProcessorName(step.name.value(), stage), stepWidget);
                        stepVBox->addWidget(nLbl);
                    }

                    stLayout->addWidget(stepWidget);
                }
            }

            // Card toolbar: Move Left, Move Right, Delete
            auto stageActionsHBox = new QHBoxLayout();

            auto moveLeftBtn = new QPushButton(stCard);
            moveLeftBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));
            moveLeftBtn->setToolTip(tr("Move stage left"));
            moveLeftBtn->setEnabled(i > 0);
            connect(moveLeftBtn, &QPushButton::clicked, [this, i]() {
                if (m_dspController && m_dspController->pipelineStore()) {
                    m_dspController->pipelineStore()->moveStage(static_cast<int>(i), static_cast<int>(i - 1));
                    m_dspController->applyConfig();
                    rebuildOverview();
                }
            });
            stageActionsHBox->addWidget(moveLeftBtn);

            auto moveRightBtn = new QPushButton(stCard);
            moveRightBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
            moveRightBtn->setToolTip(tr("Move stage right"));
            moveRightBtn->setEnabled(i + 1 < pipe->stages.size());
            connect(moveRightBtn, &QPushButton::clicked, [this, i]() {
                if (m_dspController && m_dspController->pipelineStore()) {
                    m_dspController->pipelineStore()->moveStage(static_cast<int>(i), static_cast<int>(i + 1));
                    m_dspController->applyConfig();
                    rebuildOverview();
                }
            });
            stageActionsHBox->addWidget(moveRightBtn);

            stageActionsHBox->addStretch();

            auto deleteBtn = new QPushButton(stCard);
            deleteBtn->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
            deleteBtn->setToolTip(tr("Delete stage"));
            connect(deleteBtn, &QPushButton::clicked, [this, stageId = stage.id]() {
                if (m_dspController && m_dspController->pipelineStore()) {
                    m_dspController->pipelineStore()->deleteStage(stageId);
                    m_dspController->applyConfig();
                    rebuildOverview();
                }
            });
            stageActionsHBox->addWidget(deleteBtn);

            stLayout->addLayout(stageActionsHBox);

            // Context Menu on stage card
            stCard->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(
                stCard, &QWidget::customContextMenuRequested,
                [this, i, stageId = stage.id, totalStages = pipe->stages.size()](const QPoint& pos) {
                    auto senderWidget = qobject_cast<QWidget*>(sender());
                    if (!senderWidget)
                        return;
                    QMenu menu(senderWidget);

                    auto toggleAct = menu.addAction(tr("Toggle Enabled"));
                    connect(toggleAct, &QAction::triggered, [this, i]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->stages[i].isEnabled =
                                !m_dspController->pipelineStore()->stages[i].isEnabled;
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    auto moveLeftAct = menu.addAction(tr("Move Left"));
                    moveLeftAct->setEnabled(i > 0);
                    connect(moveLeftAct, &QAction::triggered, [this, i]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->moveStage(static_cast<int>(i), static_cast<int>(i - 1));
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    auto moveRightAct = menu.addAction(tr("Move Right"));
                    moveRightAct->setEnabled(i + 1 < totalStages);
                    connect(moveRightAct, &QAction::triggered, [this, i]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->moveStage(static_cast<int>(i), static_cast<int>(i + 1));
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    auto dupAct = menu.addAction(tr("Duplicate Stage"));
                    connect(dupAct, &QAction::triggered, [this, stageId]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->duplicateStage(stageId);
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    menu.addSeparator();

                    auto deleteAct = menu.addAction(tr("Delete Stage"));
                    connect(deleteAct, &QAction::triggered, [this, stageId]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->deleteStage(stageId);
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    menu.exec(senderWidget->mapToGlobal(pos));
                });

            m_canvasLayout->addWidget(stCard, 0, Qt::AlignVCenter);
            addConnector(outCh, nextInCh, isMismatched);
        }
    }

    // 4. Playback Output Node
    auto [playCard, playLayout] = createNodeCard(tr("Output (Playback)"), QString::fromStdString(playDevName));

    auto playForm = new QFormLayout();
    if (isOutputMismatch) {
        auto warnLbl = new QLabel(tr("Got %1 ch, Device expects %2 ch").arg(finalOutputCh).arg(playbackCh), playCard);
        playForm->addRow(tr("Warning:"), warnLbl);
    }

    playForm->addRow(tr("Channels:"), new QLabel(QString::number(playbackCh), playCard));
    playForm->addRow(tr("Sample Rate:"), new QLabel(formatSampleRate(playbackRate), playCard));
    playLayout->addLayout(playForm);
    playLayout->addWidget(createScrollableChannelPills(playbackCh, playCard));

    m_canvasLayout->addWidget(playCard, 0, Qt::AlignVCenter);

    updateScrollHeight();
    QTimer::singleShot(0, this, &PipelineOverviewWidget::updateScrollHeight);
}

void PipelineOverviewWidget::updateScrollHeight() {
    if (!m_canvasLayout || !m_scrollArea || !m_canvasWidget)
        return;

    int maxCardH = 150;
    for (int i = 0; i < m_canvasLayout->count(); ++i) {
        auto item = m_canvasLayout->itemAt(i);
        if (item && item->widget()) {
            if (item->widget()->layout()) {
                item->widget()->layout()->activate();
            }
            maxCardH = std::max(maxCardH, item->widget()->sizeHint().height());
        }
    }
    m_canvasLayout->activate();
    int contentH = std::max({maxCardH, m_canvasLayout->sizeHint().height(), m_canvasWidget->sizeHint().height()});
    int sbH = (m_scrollArea->horizontalScrollBar() && m_scrollArea->horizontalScrollBar()->isVisible())
                  ? m_scrollArea->horizontalScrollBar()->height()
                  : 16;
    m_canvasWidget->setMinimumHeight(contentH);
    m_scrollArea->setFixedHeight(contentH + sbH + 16);
}

void PipelineOverviewWidget::showEvent(QShowEvent* event) {
    QGroupBox::showEvent(event);
    updateScrollHeight();
    QTimer::singleShot(0, this, &PipelineOverviewWidget::updateScrollHeight);
}
