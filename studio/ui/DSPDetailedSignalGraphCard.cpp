#include "ui/DSPDetailedSignalGraphCard.h"

#include "ui/StyleTheme.h"

#include <QPainterPath>
#include <algorithm>
#include <cmath>

// MARK: - DSPGraphCanvas Implementation

DSPGraphCanvas::DSPGraphCanvas(std::shared_ptr<DSPEngineController> dspController, QWidget* parent)
    : QWidget(parent), m_dspController(dspController) {
    setMouseTracking(true);
    rebuildGraph();
}

void DSPGraphCanvas::resetLayout() {
    m_customPositions.clear();
    update();
    emit layoutChanged();
}

QPointF DSPGraphCanvas::getBlockPos(const GraphBlock& b, qreal originY) const {
    auto it = m_customPositions.find(b.id);
    if (it != m_customPositions.end()) {
        return it->second;
    }
    return QPointF(b.x + m_canvasPadding + 40, originY + b.y);
}

QString DSPGraphCanvas::readableFilterStepName(const std::string& rawName, const PipelineStage& stage) const {
    if (rawName.find("cx5_hi") != std::string::npos)
        return "cx5_hi";
    if (rawName.find("cx5_lo_gain") != std::string::npos)
        return "cx5_lo_gain";
    if (rawName.find("cx5_lo") != std::string::npos)
        return "cx5_lo";

    QString qRaw = QString::fromStdString(rawName);
    QStringList parts = qRaw.split('_');
    QString suffix = parts.last();

    if (suffix == "preamp")
        return "preamp";
    if (suffix == "invert")
        return "invert";
    if (suffix == "lp")
        return "Linkwitz_LP";
    if (suffix == "hp")
        return "Linkwitz_HP";
    if (suffix == "conv")
        return "Convolution";
    if (suffix == "gain")
        return "Gain";
    if (suffix == "delay")
        return "Delay";

    return qRaw;
}

QString DSPGraphCanvas::readableMixerTitle(const std::string& rawName, int inCh, int outCh) const {
    if (rawName.find("2to4") != std::string::npos)
        return "2to4";
    if (rawName.find("4to2") != std::string::npos)
        return "4to2";
    if (!rawName.empty())
        return QString::fromStdString(rawName);
    return QString("%1to%2").arg(inCh).arg(outCh);
}

void DSPGraphCanvas::calculateGraphLayout() {
    m_blocks.clear();
    m_boxes.clear();
    m_arrows.clear();
    m_blocksMap.clear();

    if (!m_dspController || !m_dspController->settings() || !m_dspController->pipelineStore()) {
        setFixedSize(600, 300);
        return;
    }

    auto devMgr = m_dspController ? m_dspController->devices() : nullptr;
    auto settings = m_dspController->settings();
    auto pipe = m_dspController->pipelineStore();

    int captureChannels = devMgr ? std::max(1, devMgr->captureConfig.channels) : 2;
    int playbackChannels = devMgr ? std::max(1, devMgr->playbackConfig.channels) : 2;
    int sampleRate = devMgr ? devMgr->captureConfig.sampleRate : 48000;

    std::vector<std::vector<std::vector<GraphBlock>>> stages;
    int totalLength = 0;
    int stageStart = 0;
    int activeChannels = captureChannels;

    qreal minY = 0;
    qreal maxY = 0;

    auto registerY = [&](qreal y) {
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    };

    auto yPos = [&](int channel, int activeChannelsInStage, bool isPassthrough = false) -> qreal {
        if (isPassthrough) {
            int passthroughIdx = channel - 4;
            qreal y = (2.0 + static_cast<qreal>(passthroughIdx)) * m_yStep;
            registerY(y);
            return y;
        }
        qreal y = (-static_cast<qreal>(activeChannelsInStage) / 2.0 + 0.5 + static_cast<qreal>(channel)) * m_yStep;
        registerY(y);
        return y;
    };

    auto xPos = [&](int step) -> qreal { return static_cast<qreal>(step) * m_xStep; };

    auto makeContainerBox = [&](const QString& id, const QString& label, qreal centerX,
                                const std::vector<GraphBlock>& blocksInBox) -> ContainerBox {
        qreal minBlockY = 0;
        qreal maxBlockY = 0;
        if (!blocksInBox.empty()) {
            minBlockY = blocksInBox[0].y;
            maxBlockY = blocksInBox[0].y;
            for (const auto& b : blocksInBox) {
                minBlockY = std::min(minBlockY, b.y);
                maxBlockY = std::max(maxBlockY, b.y);
            }
        }
        qreal centerY = (minBlockY + maxBlockY) / 2.0;
        qreal height = (maxBlockY - minBlockY) + m_blockHeight + 20;

        std::vector<QString> containedIds;
        for (const auto& b : blocksInBox) {
            containedIds.push_back(b.id);
        }
        return ContainerBox{id,          label, centerX, centerY, 76, height, static_cast<int>(blocksInBox.size()),
                            containedIds};
    };

    // 1. INPUT STAGE
    std::vector<std::vector<GraphBlock>> captureStageChannels;
    std::vector<GraphBlock> captureInputBlocks;
    for (int n = 0; n < activeChannels; ++n) {
        qreal y = yPos(n, activeChannels, false);
        qreal x = xPos(0);
        GraphBlock b{QString("input_ch%1").arg(n), QString::number(n + 1), x, y, 48, m_blockHeight, true};
        m_blocks.push_back(b);
        captureInputBlocks.push_back(b);
        captureStageChannels.push_back({b});
    }

    QString captureName =
        QString::fromStdString(settings->deviceConfig.capture.coreAudio.device.value_or("Capture Input"));
    m_boxes.push_back(makeContainerBox("box_input", captureName, xPos(0), captureInputBlocks));
    stages.push_back(captureStageChannels);

    // Build presets lookup maps
    std::map<QUuid, EQPreset> eqMap;
    for (const auto& p : pipe->eqPresets)
        eqMap[p.id] = p;
    std::map<QUuid, ConvolutionPreset> convMap;
    for (const auto& p : pipe->convPresets)
        convMap[p.id] = p;

    // 2. PIPELINE STEPS LOOP
    for (size_t stageIdx = 0; stageIdx < pipe->stages.size(); ++stageIdx) {
        const auto& stage = pipe->stages[stageIdx];
        if (!stage.isEnabled || !stage.isActive())
            continue;

        int currentStageInputChannels = stages.empty() ? activeChannels : static_cast<int>(stages.back().size());

        StageBuildResult buildRes =
            StageBuilders::buildStage(stage, sampleRate, currentStageInputChannels, eqMap, convMap);
        auto stageMixers = buildRes.mixers;
        auto steps = buildRes.steps;

        std::map<int, int> stageFilterBlockCounts;

        for (const auto& step : steps) {
            if (step.type == PipelineStepType::Mixer) {
                totalLength += 1;
                std::string rawNameStr = step.name.value_or(stage.name);

                auto mixIt = stageMixers.find(rawNameStr);
                const MixerConfig* mixconf = (mixIt != stageMixers.end())
                                                 ? &mixIt->second
                                                 : (!stageMixers.empty() ? &stageMixers.begin()->second : nullptr);

                int rawOutCount = mixconf ? mixconf->channelsOut : stage.mixerChannelsOut;
                bool is2to4 = rawNameStr.find("2to4") != std::string::npos;
                bool is4to2 = rawNameStr.find("4to2") != std::string::npos;

                int outChannels = is2to4 ? 4 : (is4to2 ? 2 : rawOutCount);
                activeChannels = outChannels;

                std::vector<std::vector<GraphBlock>> mixerStageChannels;
                std::vector<GraphBlock> mixerBoxBlocks;

                for (int n = 0; n < outChannels; ++n) {
                    qreal y = yPos(n, outChannels, false);
                    qreal x = xPos(totalLength);
                    GraphBlock b{QString("mixer_%1_ch%2").arg(totalLength).arg(n),
                                 QString::number(n + 1),
                                 x,
                                 y,
                                 48,
                                 m_blockHeight,
                                 true};
                    m_blocks.push_back(b);
                    mixerBoxBlocks.push_back(b);
                    mixerStageChannels.push_back({b});
                }

                std::set<int> mappedSourcesInBox;

                if (mixconf && !mixconf->mapping.empty()) {
                    for (const auto& mapping : mixconf->mapping) {
                        int destCh = mapping.dest;
                        if (destCh >= outChannels)
                            continue;

                        for (const auto& src : mapping.sources) {
                            int srcCh = src.channel;
                            if (stages.empty() || srcCh >= static_cast<int>(stages.back().size()))
                                continue;

                            const auto& prevChBlocks = stages.back()[srcCh];
                            if (prevChBlocks.empty())
                                continue;
                            GraphBlock srcBlock = prevChBlocks.back();

                            mappedSourcesInBox.insert(srcCh);
                            GraphBlock destBlock = mixerStageChannels[destCh][0];

                            float g = static_cast<float>(src.gainValue());
                            QString labelStr = (g == 0.0f) ? "0 dB" : QString::asprintf("%+.1f dB", g);
                            if (src.inverted.value_or(false)) {
                                labelStr += "\ninv.";
                            }

                            m_arrows.push_back(GraphArrow{
                                QString("arrow_mix_%1_%2_%3").arg(totalLength).arg(srcCh).arg(destCh), srcBlock.id,
                                destBlock.id, QPointF(srcBlock.x + srcBlock.width / 2, srcBlock.y),
                                QPointF(destBlock.x - destBlock.width / 2, destBlock.y), labelStr});
                        }
                    }
                } else {
                    // Fallback 1-to-1 arrows
                    for (int n = 0; n < outChannels; ++n) {
                        int srcCh = std::min(n, static_cast<int>(stages.back().size()) - 1);
                        if (!stages.back()[srcCh].empty()) {
                            GraphBlock srcBlock = stages.back()[srcCh].back();
                            mappedSourcesInBox.insert(srcCh);
                            GraphBlock destBlock = mixerStageChannels[n][0];
                            m_arrows.push_back(
                                GraphArrow{QString("arrow_mix_fb_%1_%2").arg(totalLength).arg(n), srcBlock.id,
                                           destBlock.id, QPointF(srcBlock.x + srcBlock.width / 2, srcBlock.y),
                                           QPointF(destBlock.x - destBlock.width / 2, destBlock.y), "0 dB"});
                        }
                    }
                }

                auto nextStage = mixerStageChannels;
                if (!stages.empty()) {
                    for (size_t c = 0; c < stages.back().size(); ++c) {
                        if (mappedSourcesInBox.find(c) == mappedSourcesInBox.end()) {
                            nextStage.push_back(stages.back()[c]);
                        }
                    }
                }
                stages.push_back(nextStage);

                int mixInCh = mixconf ? mixconf->channelsIn : currentStageInputChannels;
                m_boxes.push_back(makeContainerBox(QString("box_mixer_%1").arg(totalLength),
                                                   readableMixerTitle(rawNameStr, mixInCh, outChannels),
                                                   xPos(totalLength), mixerBoxBlocks));
                stageStart = totalLength;

            } else if (step.type == PipelineStepType::Filter) {
                std::vector<int> chNbrs;
                if (!step.channels.empty()) {
                    chNbrs = step.channels;
                } else if (step.channel.has_value()) {
                    chNbrs = {step.channel.value()};
                } else {
                    for (int c = 0; c < activeChannels; ++c)
                        chNbrs.push_back(c);
                }

                std::vector<std::string> namesToUnroll;
                if (!step.names.empty()) {
                    namesToUnroll = step.names;
                } else if (step.name.has_value()) {
                    namesToUnroll = {step.name.value()};
                } else {
                    namesToUnroll = {stage.name};
                }

                for (int chNbr : chNbrs) {
                    if (stages.empty() || chNbr >= static_cast<int>(stages.back().size()))
                        continue;

                    for (const auto& rawName : namesToUnroll) {
                        QString name = readableFilterStepName(rawName, stage);
                        int countInStage = stageFilterBlockCounts[chNbr];
                        int chStep = stageStart + 1 + countInStage;
                        totalLength = std::max(totalLength, chStep);
                        stageFilterBlockCounts[chNbr] = countInStage + 1;

                        qreal y = yPos(chNbr, activeChannels, false);
                        qreal x = xPos(chStep);

                        GraphBlock b{
                            QString("filter_%1_%2_%3").arg(chStep).arg(chNbr).arg(QString::fromStdString(rawName)),
                            name,
                            x,
                            y,
                            m_blockWidth,
                            m_blockHeight,
                            false};
                        m_blocks.push_back(b);

                        if (!stages.back()[chNbr].empty()) {
                            GraphBlock srcBlock = stages.back()[chNbr].back();
                            m_arrows.push_back(GraphArrow{QString("arrow_filter_%1_%2_%3")
                                                              .arg(chStep)
                                                              .arg(chNbr)
                                                              .arg(QString::fromStdString(rawName)),
                                                          srcBlock.id, b.id,
                                                          QPointF(srcBlock.x + srcBlock.width / 2, srcBlock.y),
                                                          QPointF(b.x - b.width / 2, b.y), ""});
                        }
                        stages.back()[chNbr].push_back(b);
                    }
                }

            } else if (step.type == PipelineStepType::Processor) {
                totalLength += 1;
                QString name = QString::fromStdString(step.name.value_or(stage.name));

                std::vector<std::vector<GraphBlock>> procStageChannels;
                std::vector<GraphBlock> procBoxBlocks;

                for (int n = 0; n < activeChannels; ++n) {
                    qreal y = yPos(n, activeChannels, false);
                    qreal x = xPos(totalLength);
                    GraphBlock b{QString("proc_%1_ch%2").arg(totalLength).arg(n),
                                 QString::number(n + 1),
                                 x,
                                 y,
                                 48,
                                 m_blockHeight,
                                 true};
                    m_blocks.push_back(b);
                    procBoxBlocks.push_back(b);
                    procStageChannels.push_back({b});

                    if (!stages.back().empty() && n < static_cast<int>(stages.back().size()) &&
                        !stages.back()[n].empty()) {
                        GraphBlock srcBlock = stages.back()[n].back();
                        m_arrows.push_back(GraphArrow{QString("arrow_proc_%1_%2").arg(totalLength).arg(n), srcBlock.id,
                                                      b.id, QPointF(srcBlock.x + srcBlock.width / 2, srcBlock.y),
                                                      QPointF(b.x - b.width / 2, b.y), ""});
                    }
                }

                auto nextStage = procStageChannels;
                if (!stages.empty()) {
                    for (size_t c = activeChannels; c < stages.back().size(); ++c) {
                        nextStage.push_back(stages.back()[c]);
                    }
                }
                stages.push_back(nextStage);

                m_boxes.push_back(
                    makeContainerBox(QString("box_proc_%1").arg(totalLength), name, xPos(totalLength), procBoxBlocks));
                stageStart = totalLength;
            }
        }
        stageStart = totalLength;
    }

    // 3. PLAYBACK OUTPUT STAGE
    totalLength += 1;
    std::vector<GraphBlock> playBoxBlocks;
    for (int n = 0; n < activeChannels; ++n) {
        qreal y = yPos(n, activeChannels, false);
        qreal x = xPos(totalLength);
        GraphBlock b{QString("output_ch%1").arg(n), QString::number(n + 1), x, y, 48, m_blockHeight, true};
        m_blocks.push_back(b);
        playBoxBlocks.push_back(b);

        if (!stages.empty() && n < static_cast<int>(stages.back().size()) && !stages.back()[n].empty()) {
            GraphBlock srcBlock = stages.back()[n].back();
            m_arrows.push_back(GraphArrow{QString("arrow_play_%1").arg(n), srcBlock.id, b.id,
                                          QPointF(srcBlock.x + srcBlock.width / 2, srcBlock.y),
                                          QPointF(b.x - b.width / 2, b.y), ""});
        }
    }

    QString playName =
        QString::fromStdString(settings->deviceConfig.playback.coreAudio.device.value_or("Playback Output"));
    m_boxes.push_back(makeContainerBox("box_output", playName, xPos(totalLength), playBoxBlocks));

    for (const auto& b : m_blocks) {
        m_blocksMap[b.id] = b;
    }

    qreal totalWidth = xPos(totalLength) + m_canvasPadding * 2 + 100;
    qreal totalHeight = (maxY - minY) + m_canvasPadding * 2 + m_titleHeaderHeight + 60;

    // Check custom position overrides to adjust canvas bounds if needed
    qreal originY = totalHeight / 2.0 + m_titleHeaderHeight / 2.0;
    for (const auto& b : m_blocks) {
        QPointF pos = getBlockPos(b, originY);
        totalWidth = std::max(totalWidth, pos.x() + b.width / 2.0 + 80);
        totalHeight = std::max(totalHeight, pos.y() + b.height / 2.0 + 60);
    }

    setFixedSize(static_cast<int>(std::ceil(totalWidth)), static_cast<int>(std::ceil(totalHeight)));
    update();
}

void DSPGraphCanvas::rebuildGraph() {
    calculateGraphLayout();
}

void DSPGraphCanvas::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    qreal originY = height() / 2.0 + m_titleHeaderHeight / 2.0;
    bool isDarkTheme = StyleTheme::isDark();

    // 1. Render Container Boxes (Layer 1)
    for (const auto& box : m_boxes) {
        std::vector<GraphBlock> childBlocks;
        for (const auto& id : box.containedBlockIds) {
            auto it = m_blocksMap.find(id);
            if (it != m_blocksMap.end()) {
                childBlocks.push_back(it->second);
            }
        }

        qreal boxCenterX = box.centerX + m_canvasPadding + 40;
        qreal boxCenterY = originY + box.centerY;
        qreal boxW = box.width;
        qreal boxH = box.height;

        if (!childBlocks.empty()) {
            qreal minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
            for (const auto& b : childBlocks) {
                QPointF p = getBlockPos(b, originY);
                minX = std::min(minX, p.x() - b.width / 2.0);
                maxX = std::max(maxX, p.x() + b.width / 2.0);
                minY = std::min(minY, p.y() - b.height / 2.0);
                maxY = std::max(maxY, p.y() + b.height / 2.0);
            }
            minX -= 15;
            maxX += 15;
            minY -= 12;
            maxY += 12;

            boxCenterX = (minX + maxX) / 2.0;
            boxCenterY = (minY + maxY) / 2.0;
            boxW = std::max(box.width, maxX - minX);
            boxH = std::max(40.0, maxY - minY);
        }

        QRectF boxRect(boxCenterX - boxW / 2.0, boxCenterY - boxH / 2.0, boxW, boxH);

        QPen boxPen(isDarkTheme ? QColor(255, 255, 255, 45) : QColor(0, 0, 0, 40), 1, Qt::DashLine);
        painter.setPen(boxPen);
        painter.setBrush(isDarkTheme ? QColor(255, 255, 255, 6) : QColor(0, 0, 0, 6));
        painter.drawRoundedRect(boxRect, 10, 10);

        // Stage Title Header above Box
        painter.setFont(QFont("monospace", 10, QFont::Bold));
        painter.setPen(StyleTheme::accent());
        painter.drawText(QRectF(boxRect.left() - 40, boxRect.top() - 22, boxW + 80, 20), Qt::AlignCenter, box.label);
    }

    // 2. Render Bezier Connecting Arrows (Layer 2)
    for (const auto& arrow : m_arrows) {
        QPointF p0, p1;

        auto srcIt = m_blocksMap.find(arrow.fromBlockId);
        if (srcIt != m_blocksMap.end()) {
            QPointF pos = getBlockPos(srcIt->second, originY);
            p0 = QPointF(pos.x() + srcIt->second.width / 2.0, pos.y());
        } else {
            p0 = QPointF(arrow.fromFallback.x() + m_canvasPadding + 40, originY + arrow.fromFallback.y());
        }

        auto destIt = m_blocksMap.find(arrow.toBlockId);
        if (destIt != m_blocksMap.end()) {
            QPointF pos = getBlockPos(destIt->second, originY);
            p1 = QPointF(pos.x() - destIt->second.width / 2.0, pos.y());
        } else {
            p1 = QPointF(arrow.toFallback.x() + m_canvasPadding + 40, originY + arrow.toFallback.y());
        }

        qreal dx = p1.x() - p0.x();
        QPointF ctrl1(p0.x() + dx * 0.45, p0.y());
        QPointF ctrl2(p0.x() + dx * 0.55, p1.y());

        QPainterPath path;
        path.moveTo(p0);
        path.cubicTo(ctrl1, ctrl2, p1);

        painter.setPen(QPen(isDarkTheme ? QColor(255, 255, 255, 120) : QColor(0, 0, 0, 110), 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);

        // Arrowhead cap
        QPolygonF capPolygon;
        capPolygon << p1 << QPointF(p1.x() - 6, p1.y() - 3.5) << QPointF(p1.x() - 6, p1.y() + 3.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(isDarkTheme ? QColor(255, 255, 255, 180) : QColor(0, 0, 0, 160));
        painter.drawPolygon(capPolygon);

        // Arrow text label
        if (!arrow.label.isEmpty()) {
            QPointF midPoint(p0.x() + dx * 0.65, p0.y() + (p1.y() - p0.y()) * 0.65);
            painter.setFont(QFont("monospace", 8, QFont::DemiBold));
            painter.setPen(StyleTheme::textSecondary());
            painter.drawText(QRectF(midPoint.x() - 30, midPoint.y() - 14, 60, 20), Qt::AlignCenter, arrow.label);
        }
    }

    // 3. Render Draggable Interactive Blocks (Layer 3)
    for (const auto& b : m_blocks) {
        QPointF pos = getBlockPos(b, originY);
        QRectF bRect(pos.x() - b.width / 2.0, pos.y() - b.height / 2.0, b.width, b.height);

        if (b.isChannelPort) {
            painter.setBrush(isDarkTheme ? QColor(0, 122, 255, 35) : QColor(0, 122, 255, 30));
            painter.setPen(QPen(QColor(0, 122, 255, 80), 1));
            painter.drawRoundedRect(bRect, 6, 6);

            painter.setFont(QFont("monospace", 11, QFont::Bold));
            painter.setPen(QColor("#007aff"));
            painter.drawText(bRect, Qt::AlignCenter, b.label);
        } else if (!b.label.isEmpty()) {
            painter.setBrush(StyleTheme::cardBg());
            painter.setPen(QPen(isDarkTheme ? QColor(255, 255, 255, 65) : QColor(0, 0, 0, 65), 1));
            painter.drawRoundedRect(bRect, 6, 6);

            painter.setFont(QFont("monospace", 10, QFont::DemiBold));
            painter.setPen(StyleTheme::textPrimary());
            painter.drawText(bRect, Qt::AlignCenter, b.label);
        }
    }
}

void DSPGraphCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        qreal originY = height() / 2.0 + m_titleHeaderHeight / 2.0;
        QPointF mousePos = event->position();

        for (auto it = m_blocks.rbegin(); it != m_blocks.rend(); ++it) {
            QPointF pos = getBlockPos(*it, originY);
            QRectF bRect(pos.x() - it->width / 2.0, pos.y() - it->height / 2.0, it->width, it->height);
            if (bRect.contains(mousePos)) {
                m_draggedBlockId = it->id;
                m_dragStartPos = mousePos;
                m_blockOriginPos = pos;
                setCursor(Qt::ClosedHandCursor);
                break;
            }
        }
    }
}

void DSPGraphCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!m_draggedBlockId.isEmpty()) {
        QPointF delta = event->position() - m_dragStartPos;
        m_customPositions[m_draggedBlockId] = m_blockOriginPos + delta;
        update();
        emit layoutChanged();
    } else {
        qreal originY = height() / 2.0 + m_titleHeaderHeight / 2.0;
        QPointF mousePos = event->position();
        bool hovered = false;
        for (const auto& b : m_blocks) {
            QPointF pos = getBlockPos(b, originY);
            QRectF bRect(pos.x() - b.width / 2.0, pos.y() - b.height / 2.0, b.width, b.height);
            if (bRect.contains(mousePos)) {
                hovered = true;
                break;
            }
        }
        setCursor(hovered ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }
}

void DSPGraphCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && !m_draggedBlockId.isEmpty()) {
        m_draggedBlockId.clear();
        setCursor(Qt::OpenHandCursor);
    }
}

// MARK: - DSPDetailedSignalGraphCard Implementation

DSPDetailedSignalGraphCard::DSPDetailedSignalGraphCard(std::shared_ptr<DSPEngineController> dspController,
                                                       QWidget* parent)
    : QGroupBox("DSP Signal Processing Graph", parent), m_dspController(dspController) {
    setupUi();

    if (m_dspController) {
        if (m_dspController->settings()) {
            connect(m_dspController->settings().get(), &AudioSettings::settingsChanged, this,
                    &DSPDetailedSignalGraphCard::updateCard);
        }
        if (m_dspController->pipelineStore()) {
            connect(m_dspController->pipelineStore().get(), &PipelineStore::pipelineChanged, this,
                    &DSPDetailedSignalGraphCard::updateCard);
        }
    }

    updateCard();
}

void DSPDetailedSignalGraphCard::setupUi() {
    auto rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 16, 12, 12);
    rootLayout->setSpacing(12);

    // Header bar
    auto headerBox = new QHBoxLayout();

    auto titleVBox = new QVBoxLayout();
    titleVBox->setSpacing(2);

    auto topRow = new QHBoxLayout();
    auto titleLbl = new QLabel("🔗 DSP Signal Processing Graph", this);
    titleLbl->setFont(QFont("System", 13, QFont::Bold));
    topRow->addWidget(titleLbl);

    m_resetLayoutBtn = new QPushButton("Reset Layout", this);
    m_resetLayoutBtn->setCursor(Qt::PointingHandCursor);
    m_resetLayoutBtn->setStyleSheet("background: rgba(128, 128, 128, 0.15); color: auto; border: none; "
                                    "border-radius: 12px; padding: 3px 8px; font-size: 10px; font-weight: bold;");
    m_resetLayoutBtn->setVisible(false);
    topRow->addWidget(m_resetLayoutBtn);
    topRow->addStretch();

    titleVBox->addLayout(topRow);
    headerBox->addLayout(titleVBox, 1);

    m_activeStagesBadge = new QLabel(this);
    m_activeStagesBadge->setStyleSheet("background: rgba(0, 122, 255, 0.12); color: #007aff; border-radius: 10px; "
                                       "padding: 3px 8px; font-size: 10px; font-weight: bold;");
    headerBox->addWidget(m_activeStagesBadge, 0, Qt::AlignVCenter);

    rootLayout->addLayout(headerBox);

    // Scrollable 2D Canvas Area
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    m_canvas = new DSPGraphCanvas(m_dspController, m_scrollArea);
    connect(m_canvas, &DSPGraphCanvas::layoutChanged, [this]() {
        if (m_resetLayoutBtn)
            m_resetLayoutBtn->setVisible(m_canvas->hasCustomPositions());
    });

    connect(m_resetLayoutBtn, &QPushButton::clicked, [this]() {
        if (m_canvas)
            m_canvas->resetLayout();
        m_resetLayoutBtn->setVisible(false);
    });

    m_scrollArea->setWidget(m_canvas);
    rootLayout->addWidget(m_scrollArea);
}

void DSPDetailedSignalGraphCard::updateCard() {
    int activeCount = 0;
    if (m_dspController && m_dspController->pipelineStore()) {
        for (const auto& st : m_dspController->pipelineStore()->stages) {
            if (st.isEnabled && st.isActive())
                activeCount++;
        }
    }

    if (m_activeStagesBadge) {
        m_activeStagesBadge->setText(QString("%1 Active Stages").arg(activeCount));
    }

    if (m_canvas) {
        m_canvas->rebuildGraph();
        if (m_resetLayoutBtn)
            m_resetLayoutBtn->setVisible(m_canvas->hasCustomPositions());
        if (m_scrollArea) {
            int targetH = std::clamp(m_canvas->height() + 20, 220, 500);
            m_scrollArea->setFixedHeight(targetH);
        }
    }
}
