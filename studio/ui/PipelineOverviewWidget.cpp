#include "ui/PipelineOverviewWidget.h"

#include "ui/StyleTheme.h"

#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

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
    rootLayout->setSpacing(12);

    // 1. Header Bar
    auto headerBox = new QHBoxLayout();

    auto titleVBox = new QVBoxLayout();
    titleVBox->setSpacing(2);

    auto topTitleRow = new QHBoxLayout();
    m_headerTitle = new QLabel("📐 Signal Chain", this);
    m_headerTitle->setFont(QFont("System", 13, QFont::Bold));
    topTitleRow->addWidget(m_headerTitle);

    m_warningBadge = new QLabel("Broken Chain", this);
    m_warningBadge->setStyleSheet("background-color: rgba(255, 59, 48, 0.15); color: #ff3b30; border: 1px solid "
                                  "rgba(255, 59, 48, 0.4); border-radius: 4px; padding: 2px 6px; font-size: 10px; "
                                  "font-weight: bold;");
    m_warningBadge->setVisible(false);
    topTitleRow->addWidget(m_warningBadge);
    topTitleRow->addStretch();

    titleVBox->addLayout(topTitleRow);

    m_statsLabel = new QLabel(this);
    m_statsLabel->setStyleSheet("color: #8e8e93; font-size: 11px;");
    titleVBox->addWidget(m_statsLabel);

    headerBox->addLayout(titleVBox, 1);

    m_toggleDetailsBtn = new QPushButton("Hide Elementary Steps", this);
    m_toggleDetailsBtn->setCursor(Qt::PointingHandCursor);
    m_toggleDetailsBtn->setStyleSheet("background: rgba(0, 122, 255, 0.15); color: #007aff; border: none; "
                                      "border-radius: 12px; padding: 4px 10px; font-size: 11px; font-weight: bold;");
    connect(m_toggleDetailsBtn, &QPushButton::clicked, [this]() {
        m_showElementaryDetails = !m_showElementaryDetails;
        m_toggleDetailsBtn->setText(m_showElementaryDetails ? "Hide Elementary Steps" : "Show Elementary Steps");
        m_toggleDetailsBtn->setStyleSheet(
            m_showElementaryDetails
                ? "background: rgba(0, 122, 255, 0.15); color: #007aff; border: none; border-radius: 12px; padding: "
                  "4px 10px; font-size: 11px; font-weight: bold;"
                : "background: rgba(142, 142, 147, 0.15); color: #8e8e93; border: none; border-radius: 12px; padding: "
                  "4px 10px; font-size: 11px; font-weight: bold;");
        rebuildOverview();
    });
    headerBox->addWidget(m_toggleDetailsBtn, 0, Qt::AlignVCenter);

    rootLayout->addLayout(headerBox);

    // 2. Scrollable Canvas
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_canvasWidget = new QWidget(m_scrollArea);
    m_canvasLayout = new QHBoxLayout(m_canvasWidget);
    m_canvasLayout->setContentsMargins(4, 8, 4, 8);
    m_canvasLayout->setSpacing(0);
    m_canvasLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_scrollArea->setWidget(m_canvasWidget);
    rootLayout->addWidget(m_scrollArea);
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
    if (suffix == "limiter")
        return QString("Peak Limiter (%1 dB)").arg(QString::asprintf("%+.1f", stage.limiterLimit));
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

    if (pipe) {
        std::map<QUuid, EQPreset> eqMap;
        for (const auto& p : pipe->eqPresets)
            eqMap[p.id] = p;
        std::map<QUuid, ConvolutionPreset> convMap;
        for (const auto& p : pipe->convPresets)
            convMap[p.id] = p;

        int chTracker = captureCh;
        for (size_t i = 0; i < pipe->stages.size(); ++i) {
            const auto& st = pipe->stages[i];
            if (st.isEnabled)
                activeStageCount++;

            int inCh = pipe->channelCountBeforeStage(i, captureCh);
            auto res = StageBuilders::buildStage(st, captureRate, inCh, eqMap, convMap);
            totalElementarySteps += res.steps.size();
        }
    }

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

        auto arrowLabel = new QLabel(isMismatch ? "❌" : "➔", connWidget);
        arrowLabel->setAlignment(Qt::AlignCenter);
        arrowLabel->setStyleSheet(isMismatch ? "color: #ff3b30; font-size: 11px;"
                                             : "color: rgba(0, 122, 255, 0.8); font-size: 11px;");
        connVBox->addWidget(arrowLabel);

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
        tagLabel->setStyleSheet(
            isMismatch ? "background: rgba(255, 59, 48, 0.15); color: #ff3b30; font-size: 8px; font-weight: bold; "
                         "padding: 1px 3px; border-radius: 3px;"
                       : "background: rgba(128, 128, 128, 0.12); color: #8e8e93; font-size: 8px; font-weight: bold; "
                         "padding: 1px 3px; border-radius: 3px;");
        connVBox->addWidget(tagLabel);

        m_canvasLayout->addWidget(connWidget, 0, Qt::AlignVCenter);
    };

    // Helper: Build Graph Node Card Box
    auto createNodeCard = [this, isRunning](const QString& title, const QString& subtitle, const QString& iconStr,
                                            const QString& colorHex, bool isActive, bool isWarning = false) {
        auto card = new QWidget(m_canvasWidget);
        card->setFixedWidth(210);
        card->setStyleSheet(
            QString("QWidget { background: %1; border: %2 solid %3; border-radius: 10px; }")
                .arg(isWarning ? "rgba(255, 59, 48, 0.08)"
                               : (isActive ? QString("rgba(%1, 0.06)").arg(colorHex) : "rgba(142, 142, 147, 0.04)"))
                .arg(isWarning ? "1.5px" : "1.0px")
                .arg(isWarning ? "#ff3b30"
                               : (isActive ? QString("rgb(%1)").arg(colorHex) : "rgba(142, 142, 147, 0.2)")));

        auto cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 10, 10, 10);
        cardLayout->setSpacing(8);

        // Header Row
        auto headerRow = new QHBoxLayout();
        headerRow->setSpacing(8);

        auto iconDot = new QLabel(iconStr, card);
        iconDot->setFixedSize(26, 26);
        iconDot->setAlignment(Qt::AlignCenter);
        iconDot->setStyleSheet(
            QString("background: %1; border-radius: 13px; font-size: 12px;")
                .arg(isWarning ? "rgba(255, 59, 48, 0.2)"
                               : (isActive ? QString("rgba(%1, 0.2)").arg(colorHex) : "rgba(142, 142, 147, 0.15)")));
        headerRow->addWidget(iconDot);

        auto titleVBox = new QVBoxLayout();
        titleVBox->setSpacing(1);
        auto titleLbl = new QLabel(title, card);
        titleLbl->setFont(QFont("System", 11, QFont::Bold));
        titleLbl->setStyleSheet(isWarning ? "color: #ff3b30;" : "color: auto;");
        titleVBox->addWidget(titleLbl);

        auto subLbl = new QLabel(subtitle, card);
        subLbl->setStyleSheet("color: #8e8e93; font-size: 10px;");
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
    capInfoLbl->setStyleSheet("color: #8e8e93; font-size: 10px;");
    capLayout->addWidget(capInfoLbl);

    auto capPillsHBox = new QHBoxLayout();
    capPillsHBox->setSpacing(3);
    for (int c = 0; c < captureCh; ++c) {
        auto pill = new QLabel(QString::number(c + 1), capCard);
        pill->setAlignment(Qt::AlignCenter);
        pill->setStyleSheet("background: rgba(0, 122, 255, 0.18); color: #007aff; font-size: 9px; font-weight: bold; "
                            "border-radius: 8px; padding: 2px 6px;");
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
        resampSub->setStyleSheet("color: #8e8e93; font-size: 10px; font-family: monospace;");
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

            QString colorHex = "0, 122, 255";
            if (stage.type == StageType::MatrixMixer)
                colorHex = "175, 82, 222";
            else if (stage.type == StageType::Compressor || stage.type == StageType::NoiseGate)
                colorHex = "255, 149, 0";

            auto [stCard, stLayout] = createNodeCard(
                QString::fromStdString(stage.name), QString::fromStdString(stageTypeToString(stage.type)),
                QString::fromStdString(stageTypeToIcon(stage.type)), colorHex, active);

            // Channel overview badge
            auto chBadge = new QLabel(inCh != outCh ? QString(" ⚙️ %1 In ➔ %2 Out ").arg(inCh).arg(outCh)
                                                    : QString(" ⚙️ %1 Ch ").arg(inCh),
                                      stCard);
            chBadge->setStyleSheet("background: rgba(128, 128, 128, 0.12); color: auto; font-size: 10px; font-weight: "
                                   "500; border-radius: 4px; padding: 2px 4px;");
            stLayout->addWidget(chBadge);

            // Elementary Steps breakdown
            if (m_showElementaryDetails && active) {
                auto divider = new QFrame(stCard);
                divider->setFrameShape(QFrame::HLine);
                divider->setStyleSheet("background: rgba(128, 128, 128, 0.2); height: 1px;");
                stLayout->addWidget(divider);

                auto stepsRes = StageBuilders::buildStage(stage, captureRate, inCh, eqMap, convMap);

                for (const auto& step : stepsRes.steps) {
                    auto stepWidget = new QWidget(stCard);
                    stepWidget->setStyleSheet("QWidget { background: rgba(0, 122, 255, 0.08); border-radius: 6px; "
                                              "border: 1px solid rgba(0, 122, 255, 0.2); }");
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
                    iconLbl->setStyleSheet("font-size: 9px;");
                    stepHeader->addWidget(iconLbl);

                    QString stepTitle = "Filter Chain";
                    if (step.type == PipelineStepType::Mixer)
                        stepTitle = "Matrix / Routing Mixer";
                    else if (step.type == PipelineStepType::Processor)
                        stepTitle = "Dynamics Processor";

                    auto titleLbl = new QLabel(stepTitle, stepWidget);
                    titleLbl->setFont(QFont("System", 9, QFont::Bold));
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
                    for (int ch : chList) {
                        auto chTag = new QLabel(QString::number(ch + 1), stepWidget);
                        chTag->setAlignment(Qt::AlignCenter);
                        chTag->setStyleSheet(
                            "background: rgba(0, 122, 255, 0.2); color: #007aff; font-size: 8px; font-weight: bold; "
                            "border-radius: 3px; padding: 1px 3px;");
                        chTagsBox->addWidget(chTag);
                    }
                    stepHeader->addLayout(chTagsBox);
                    stepVBox->addLayout(stepHeader);

                    // Unrolled Step filter / mixer names
                    if (!step.names.empty()) {
                        for (const auto& rawN : step.names) {
                            auto nLbl = new QLabel(QString(" • %1").arg(readableFilterName(rawN, stage)), stepWidget);
                            nLbl->setStyleSheet("color: #8e8e93; font-size: 9px; font-family: monospace;");
                            stepVBox->addWidget(nLbl);
                        }
                    } else if (step.name.has_value()) {
                        auto nLbl = new QLabel(readableMixerOrProcessorName(step.name.value(), stage), stepWidget);
                        nLbl->setStyleSheet("color: #8e8e93; font-size: 9px; font-family: monospace;");
                        stepVBox->addWidget(nLbl);
                    }

                    stLayout->addWidget(stepWidget);
                }
            }

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
        warnLbl->setStyleSheet("color: #ff3b30; font-size: 9px; font-weight: bold;");
        playLayout->addWidget(warnLbl);
    }

    auto playInfoLbl =
        new QLabel(QString("Channels: %1 • %2").arg(playbackCh).arg(formatSampleRate(playbackRate)), playCard);
    playInfoLbl->setStyleSheet("color: #8e8e93; font-size: 10px;");
    playLayout->addWidget(playInfoLbl);

    auto playPillsHBox = new QHBoxLayout();
    playPillsHBox->setSpacing(3);
    for (int c = 0; c < playbackCh; ++c) {
        auto pill = new QLabel(QString::number(c + 1), playCard);
        pill->setAlignment(Qt::AlignCenter);
        pill->setStyleSheet("background: rgba(52, 199, 89, 0.18); color: #34c759; font-size: 9px; font-weight: bold; "
                            "border-radius: 8px; padding: 2px 6px;");
        playPillsHBox->addWidget(pill);
    }
    playPillsHBox->addStretch();
    playLayout->addLayout(playPillsHBox);

    m_canvasLayout->addWidget(playCard);

    m_canvasWidget->adjustSize();
    int contentH = m_canvasWidget->sizeHint().height();
    m_scrollArea->setFixedHeight(std::max(110, contentH + 20));
}
