#include "ui/PipelineOverviewWidget.h"

#include <QAction>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QScrollArea>
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
    rootLayout->setContentsMargins(12, 16, 12, 12);
    rootLayout->setSpacing(10);

    // 1. Header Bar
    auto headerBox = new QHBoxLayout();

    auto titleVBox = new QVBoxLayout();
    titleVBox->setSpacing(2);

    auto topTitleRow = new QHBoxLayout();
    m_headerTitle = new QLabel("Signal Chain", this);
    m_headerTitle->setFont(QFont("", 13, QFont::Bold));
    topTitleRow->addWidget(m_headerTitle);

    m_warningBadge = new QLabel("⚠️ Broken Chain", this);
    m_warningBadge->setVisible(false);
    topTitleRow->addWidget(m_warningBadge);
    topTitleRow->addStretch();

    titleVBox->addLayout(topTitleRow);

    m_statsLabel = new QLabel(this);
    titleVBox->addWidget(m_statsLabel);

    headerBox->addLayout(titleVBox, 1);

    // Categorized Add Stage Menu Button
    m_addStageBtn = new QPushButton("Add Stage…", this);
    headerBox->addWidget(m_addStageBtn, 0, Qt::AlignVCenter);

    buildAddStageMenu();

    // Details Toggle Button
    m_toggleDetailsBtn = new QPushButton("Hide Elementary Steps", this);
    connect(m_toggleDetailsBtn, &QPushButton::clicked, [this]() {
        m_showElementaryDetails = !m_showElementaryDetails;
        m_toggleDetailsBtn->setText(m_showElementaryDetails ? "Hide Elementary Steps" : "Show Elementary Steps");
        rebuildOverview();
    });
    headerBox->addWidget(m_toggleDetailsBtn, 0, Qt::AlignVCenter);

    rootLayout->addLayout(headerBox);

    // 2. Channel Signal Wires Legend Bar
    auto legendRow = new QHBoxLayout();
    legendRow->setContentsMargins(4, 0, 4, 0);
    legendRow->setSpacing(8);

    auto legendTitle = new QLabel("Channel Signal Wires:", this);
    legendTitle->setFont(QFont("", 10, QFont::Bold));
    legendRow->addWidget(legendTitle);

    m_legendContainerWidget = new QWidget(this);
    m_legendBarLayout = new QHBoxLayout(m_legendContainerWidget);
    m_legendBarLayout->setContentsMargins(0, 0, 0, 0);
    m_legendBarLayout->setSpacing(6);
    m_legendBarLayout->setAlignment(Qt::AlignLeft);
    legendRow->addWidget(m_legendContainerWidget, 1);

    rootLayout->addLayout(legendRow);

    // 3. Scrollable Canvas
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_canvasWidget = new OverviewCanvasWidget(this, m_scrollArea);
    m_canvasLayout = new QHBoxLayout(m_canvasWidget);
    m_canvasLayout->setContentsMargins(4, 8, 4, 8);
    m_canvasLayout->setSpacing(0);
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
            QString iconStr = QString::fromStdString(stageTypeToIcon(type));
            auto action = subMenu->addAction(QString("%1  %2").arg(iconStr).arg(name));
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

    addCategorySubMenu("Processors", {StageType::Compressor, StageType::NoiseGate, StageType::RACE});

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
        connWidget->setFixedWidth(52);
        auto connVBox = new QVBoxLayout(connWidget);
        connVBox->setContentsMargins(0, 0, 0, 0);
        connVBox->setSpacing(2);

        auto arrowHBox = new QHBoxLayout();
        arrowHBox->setContentsMargins(0, 0, 0, 0);
        arrowHBox->setSpacing(2);
        arrowHBox->setAlignment(Qt::AlignCenter);

        auto connLine = new QFrame(connWidget);
        connLine->setFrameShape(QFrame::HLine);
        connLine->setFixedHeight(2);
        connLine->setFixedWidth(24);
        arrowHBox->addWidget(connLine);

        auto arrowLabel = new QLabel(isMismatch ? "⚠️" : "›", connWidget);
        arrowLabel->setAlignment(Qt::AlignCenter);
        arrowHBox->addWidget(arrowLabel);
        connVBox->addLayout(arrowHBox);

        QString labelText;
        if (isMismatch) {
            labelText = QString("%1 ch ❌").arg(fromCh);
        } else if (fromCh != toCh) {
            labelText = QString("%1➔%2 ch").arg(fromCh).arg(toCh);
        } else {
            labelText = QString("%1 ch").arg(fromCh);
        }

        auto tagLabel = new QLabel(labelText, connWidget);
        tagLabel->setAlignment(Qt::AlignCenter);
        connVBox->addWidget(tagLabel);

        m_canvasLayout->addWidget(connWidget, 0, Qt::AlignVCenter);
    };

    // Helper: Build Graph Node Card Box
    auto createNodeCard = [this, isRunning](const QString& title, const QString& subtitle, const QString& iconStr,
                                            const QString& /*colorHex*/, bool /*isActive*/, bool isWarning = false) {
        auto card = new QFrame(m_canvasWidget);
        card->setFrameShape(QFrame::StyledPanel);
        card->setFrameShadow(QFrame::Sunken);
        card->setFixedWidth(210);

        auto cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 10, 10, 10);
        cardLayout->setSpacing(8);

        // Header Row
        auto headerRow = new QHBoxLayout();
        headerRow->setSpacing(8);

        auto iconDot = new QLabel(iconStr, card);
        iconDot->setFixedSize(26, 26);
        iconDot->setAlignment(Qt::AlignCenter);
        headerRow->addWidget(iconDot);

        auto titleVBox = new QVBoxLayout();
        titleVBox->setSpacing(1);
        auto titleLbl = new QLabel(title, card);
        titleLbl->setFont(QFont("", 12, QFont::Bold));
        titleVBox->addWidget(titleLbl);

        auto subLbl = new QLabel(subtitle, card);
        titleVBox->addWidget(subLbl);

        headerRow->addLayout(titleVBox, 1);
        cardLayout->addLayout(headerRow);

        return std::make_pair(card, cardLayout);
    };

    // 1. Capture Input Node
    auto [capCard, capLayout] =
        createNodeCard("Input (Capture)", QString::fromStdString(capDevName), "🎤", "0, 122, 255", isRunning);

    auto capInfoLbl =
        new QLabel(QString("Channels: %1 • %2").arg(captureCh).arg(formatSampleRate(captureRate)), capCard);
    capLayout->addWidget(capInfoLbl);

    auto capPillsHBox = new QHBoxLayout();
    capPillsHBox->setSpacing(3);
    for (int c = 0; c < captureCh; ++c) {
        auto pill = new QLabel(QString::number(c + 1), capCard);
        pill->setAlignment(Qt::AlignCenter);
        capPillsHBox->addWidget(pill);
    }
    capPillsHBox->addStretch();
    capLayout->addLayout(capPillsHBox);

    m_canvasLayout->addWidget(capCard);

    // Connector 1
    addConnector(captureCh, captureCh);

    // 2. Resampler Node (if active)
    bool resampEnabled = settings ? settings->resamplerEnabled : false;
    if (resampEnabled) {
        auto [resampCard, resampLayout] = createNodeCard("Resampler", "Synchronous SRC", "🔄", "0, 122, 255", true);

        auto resampSub = new QLabel(
            QString("All %1 channels\nTarget: %2").arg(captureCh).arg(formatSampleRate(captureRate)), resampCard);
        resampLayout->addWidget(resampSub);

        m_canvasLayout->addWidget(resampCard);
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

            StageCategory cat = stageTypeToCategory(stage.type);
            QString colorHex = categoryColorHex(cat);

            auto [stCard, stLayout] = createNodeCard(
                QString::fromStdString(stage.name), QString::fromStdString(stageTypeToString(stage.type)),
                QString::fromStdString(stageTypeToIcon(stage.type)), colorHex, active);

            // Add Stage Bypass Dot button in header row
            auto headerLayout = qobject_cast<QHBoxLayout*>(stLayout->itemAt(0)->layout());
            if (headerLayout) {
                auto bypassDotBtn = new QPushButton(active ? "🟢" : "⚪", stCard);
                bypassDotBtn->setFixedSize(16, 16);
                bypassDotBtn->setFlat(true);
                bypassDotBtn->setToolTip(active ? "Click to disable stage" : "Click to enable stage");
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

            // Channel overview badge
            auto chBadge = new QLabel(inCh != outCh ? QString(" ⚙️ %1 In ➔ %2 Out ").arg(inCh).arg(outCh)
                                                    : QString(" ⚙️ %1 Ch ").arg(inCh),
                                      stCard);
            stLayout->addWidget(chBadge);

            // Elementary Steps breakdown
            if (m_showElementaryDetails && active) {
                auto divider = new QFrame(stCard);
                divider->setFrameShape(QFrame::HLine);
                divider->setFrameShadow(QFrame::Sunken);
                stLayout->addWidget(divider);

                auto stepsRes = StageBuilders::buildStage(stage, captureRate, inCh, eqMap, convMap);

                for (const auto& step : stepsRes.steps) {
                    auto stepWidget = new QFrame(stCard);
                    stepWidget->setFrameShape(QFrame::Box);
                    stepWidget->setFrameShadow(QFrame::Plain);
                    auto stepVBox = new QVBoxLayout(stepWidget);
                    stepVBox->setContentsMargins(6, 4, 6, 4);
                    stepVBox->setSpacing(2);

                    auto stepHeader = new QHBoxLayout();
                    stepHeader->setSpacing(4);

                    QString icon = "⚙️";
                    if (step.type == PipelineStepType::Filter)
                        icon = "🎛️";
                    else if (step.type == PipelineStepType::Mixer)
                        icon = "🔀";
                    else if (step.type == PipelineStepType::Processor)
                        icon = "💻";

                    auto iconLbl = new QLabel(icon, stepWidget);
                    stepHeader->addWidget(iconLbl);

                    QString stepTitle = stepTypeTitle(step.type);
                    auto titleLbl = new QLabel(stepTitle, stepWidget);
                    titleLbl->setFont(QFont("", 9, QFont::Bold));
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
                    chTagsBox->setSpacing(2);
                    if (chList.size() == static_cast<size_t>(inCh) && inCh > 4) {
                        auto chTag = new QLabel("All", stepWidget);
                        chTag->setAlignment(Qt::AlignCenter);
                        chTagsBox->addWidget(chTag);
                    } else if (chList.size() > 4) {
                        bool isContiguous = true;
                        for (size_t i = 1; i < chList.size(); ++i) {
                            if (chList[i] != chList[i - 1] + 1) {
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
                            itemHBox->setContentsMargins(12, 0, 0, 0);
                            itemHBox->setSpacing(4);

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
            stageActionsHBox->setContentsMargins(0, 4, 0, 0);
            stageActionsHBox->setSpacing(4);

            auto moveLeftBtn = new QPushButton("◀", stCard);
            moveLeftBtn->setFixedSize(24, 22);
            moveLeftBtn->setToolTip("Move stage left");
            moveLeftBtn->setEnabled(i > 0);
            connect(moveLeftBtn, &QPushButton::clicked, [this, i]() {
                if (m_dspController && m_dspController->pipelineStore()) {
                    m_dspController->pipelineStore()->moveStage(static_cast<int>(i), static_cast<int>(i - 1));
                    m_dspController->applyConfig();
                    rebuildOverview();
                }
            });
            stageActionsHBox->addWidget(moveLeftBtn);

            auto moveRightBtn = new QPushButton("▶", stCard);
            moveRightBtn->setFixedSize(24, 22);
            moveRightBtn->setToolTip("Move stage right");
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

            auto deleteBtn = new QPushButton("🗑", stCard);
            deleteBtn->setFixedSize(24, 22);
            deleteBtn->setToolTip("Delete stage");
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

                    auto toggleAct = menu.addAction("Toggle Enabled");
                    connect(toggleAct, &QAction::triggered, [this, i]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->stages[i].isEnabled =
                                !m_dspController->pipelineStore()->stages[i].isEnabled;
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    auto moveLeftAct = menu.addAction("Move Left");
                    moveLeftAct->setEnabled(i > 0);
                    connect(moveLeftAct, &QAction::triggered, [this, i]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->moveStage(static_cast<int>(i), static_cast<int>(i - 1));
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    auto moveRightAct = menu.addAction("Move Right");
                    moveRightAct->setEnabled(i + 1 < totalStages);
                    connect(moveRightAct, &QAction::triggered, [this, i]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->moveStage(static_cast<int>(i), static_cast<int>(i + 1));
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    auto dupAct = menu.addAction("Duplicate Stage");
                    connect(dupAct, &QAction::triggered, [this, stageId]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->duplicateStage(stageId);
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    menu.addSeparator();

                    auto deleteAct = menu.addAction("Delete Stage");
                    connect(deleteAct, &QAction::triggered, [this, stageId]() {
                        if (m_dspController && m_dspController->pipelineStore()) {
                            m_dspController->pipelineStore()->deleteStage(stageId);
                            m_dspController->applyConfig();
                            rebuildOverview();
                        }
                    });

                    menu.exec(senderWidget->mapToGlobal(pos));
                });

            m_canvasLayout->addWidget(stCard);
            addConnector(outCh, nextInCh, isMismatched);
        }
    }

    // 4. Playback Output Node
    auto [playCard, playLayout] = createNodeCard("Output (Playback)", QString::fromStdString(playDevName), "🔊",
                                                 "52, 199, 89", isRunning, isOutputMismatch);

    if (isOutputMismatch) {
        auto warnLbl =
            new QLabel(QString("Got %1 ch, Device expects %2 ch").arg(finalOutputCh).arg(playbackCh), playCard);
        playLayout->addWidget(warnLbl);
    }

    auto playInfoLbl =
        new QLabel(QString("Channels: %1 • %2").arg(playbackCh).arg(formatSampleRate(playbackRate)), playCard);
    playLayout->addWidget(playInfoLbl);

    auto playPillsHBox = new QHBoxLayout();
    playPillsHBox->setSpacing(3);
    for (int c = 0; c < playbackCh; ++c) {
        auto pill = new QLabel(QString::number(c + 1), playCard);
        pill->setAlignment(Qt::AlignCenter);
        playPillsHBox->addWidget(pill);
    }
    playPillsHBox->addStretch();
    playLayout->addLayout(playPillsHBox);

    m_canvasLayout->addWidget(playCard);

    m_canvasWidget->adjustSize();
    int contentH = m_canvasWidget->sizeHint().height();
    m_scrollArea->setFixedHeight(std::max(110, contentH + 20));
}
