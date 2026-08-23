#include "ui/DSPDetailedSignalGraphCard.h"

#include <QFontDatabase>
#include <QPainterPath>
#include <QScrollBar>
#include <QTimer>
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

QString DSPGraphCanvas::readableFilterStepName(const std::string& rawName) const {
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

QSizeF DSPGraphCanvas::calculateBlockSize(const QString& label, bool isChannelPort) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (isChannelPort) {
        font.setBold(true);
    }
    QFontMetricsF fm(font);
    QRectF textRect = fm.boundingRect(QRectF(0, 0, 10000, 10000), Qt::AlignCenter, label);
    qreal textWidth = std::max(fm.horizontalAdvance(label), textRect.width());
    qreal textHeight = fm.height();

    qreal minWidth = isChannelPort ? 48.0 : 85.0;
    qreal paddingH = isChannelPort ? 16.0 : 24.0;
    qreal width = std::max(minWidth, textWidth + paddingH);
    qreal height = std::max(28.0, textHeight + 8.0);
    return QSizeF(width, height);
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

    auto makeContainerBox = [&](const QString& id, const QString& label, int stepIndex,
                                const std::vector<GraphBlock>& blocksInBox) -> ContainerBox {
        qreal minBlockY = 0;
        qreal maxBlockY = 0;
        qreal maxBlockW = 48.0;
        if (!blocksInBox.empty()) {
            minBlockY = blocksInBox[0].y;
            maxBlockY = blocksInBox[0].y;
            maxBlockW = blocksInBox[0].width;
            for (const auto& b : blocksInBox) {
                minBlockY = std::min(minBlockY, b.y);
                maxBlockY = std::max(maxBlockY, b.y);
                maxBlockW = std::max(maxBlockW, b.width);
            }
        }
        qreal centerY = (minBlockY + maxBlockY) / 2.0;
        qreal height = (maxBlockY - minBlockY) + m_blockHeight + 20;

        QFont headerFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        headerFont.setBold(true);
        QFontMetricsF fmHeader(headerFont);
        qreal titleWidth = fmHeader.horizontalAdvance(label);
        qreal width = std::max({76.0, maxBlockW + 28.0, titleWidth + 24.0});

        std::vector<QString> containedIds;
        for (const auto& b : blocksInBox) {
            containedIds.push_back(b.id);
        }
        return ContainerBox{id,           label,    0, centerY, width, height, static_cast<int>(blocksInBox.size()),
                            containedIds, stepIndex};
    };

    // 1. INPUT STAGE
    std::vector<std::vector<GraphBlock>> captureStageChannels;
    std::vector<GraphBlock> captureInputBlocks;
    for (int n = 0; n < activeChannels; ++n) {
        qreal y = yPos(n, activeChannels, false);
        QString label = QString::number(n + 1);
        QSizeF sz = calculateBlockSize(label, true);
        GraphBlock b{QString("input_ch%1").arg(n), label, 0, y, sz.width(), sz.height(), true, 0};
        m_blocks.push_back(b);
        captureInputBlocks.push_back(b);
        captureStageChannels.push_back({b});
    }

    QString captureName = QString::fromStdString(settings->deviceConfig.capture.deviceName().value_or("Capture Input"));
    m_boxes.push_back(makeContainerBox("box_input", captureName, 0, captureInputBlocks));
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
                    QString label = QString::number(n + 1);
                    QSizeF sz = calculateBlockSize(label, true);
                    GraphBlock b{QString("mixer_%1_ch%2").arg(totalLength).arg(n),
                                 label,
                                 0,
                                 y,
                                 sz.width(),
                                 sz.height(),
                                 true,
                                 totalLength};
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
                                destBlock.id, QPointF(0, srcBlock.y), QPointF(0, destBlock.y), labelStr});
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
                            m_arrows.push_back(GraphArrow{QString("arrow_mix_fb_%1_%2").arg(totalLength).arg(n),
                                                          srcBlock.id, destBlock.id, QPointF(0, srcBlock.y),
                                                          QPointF(0, destBlock.y), "0 dB"});
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
                                                   readableMixerTitle(rawNameStr, mixInCh, outChannels), totalLength,
                                                   mixerBoxBlocks));
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
                        QString name = readableFilterStepName(rawName);
                        int countInStage = stageFilterBlockCounts[chNbr];
                        int chStep = stageStart + 1 + countInStage;
                        totalLength = std::max(totalLength, chStep);
                        stageFilterBlockCounts[chNbr] = countInStage + 1;

                        qreal y = yPos(chNbr, activeChannels, false);
                        QSizeF sz = calculateBlockSize(name, false);

                        GraphBlock b{
                            QString("filter_%1_%2_%3").arg(chStep).arg(chNbr).arg(QString::fromStdString(rawName)),
                            name,
                            0,
                            y,
                            sz.width(),
                            sz.height(),
                            false,
                            chStep};
                        m_blocks.push_back(b);

                        if (!stages.back()[chNbr].empty()) {
                            GraphBlock srcBlock = stages.back()[chNbr].back();
                            m_arrows.push_back(GraphArrow{QString("arrow_filter_%1_%2_%3")
                                                              .arg(chStep)
                                                              .arg(chNbr)
                                                              .arg(QString::fromStdString(rawName)),
                                                          srcBlock.id, b.id, QPointF(0, srcBlock.y), QPointF(0, b.y),
                                                          ""});
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
                    QString label = QString::number(n + 1);
                    QSizeF sz = calculateBlockSize(label, true);
                    GraphBlock b{QString("proc_%1_ch%2").arg(totalLength).arg(n),
                                 label,
                                 0,
                                 y,
                                 sz.width(),
                                 sz.height(),
                                 true,
                                 totalLength};
                    m_blocks.push_back(b);
                    procBoxBlocks.push_back(b);
                    procStageChannels.push_back({b});

                    if (!stages.back().empty() && n < static_cast<int>(stages.back().size()) &&
                        !stages.back()[n].empty()) {
                        GraphBlock srcBlock = stages.back()[n].back();
                        m_arrows.push_back(GraphArrow{QString("arrow_proc_%1_%2").arg(totalLength).arg(n), srcBlock.id,
                                                      b.id, QPointF(0, srcBlock.y), QPointF(0, b.y), ""});
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
                    makeContainerBox(QString("box_proc_%1").arg(totalLength), name, totalLength, procBoxBlocks));
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
        QString label = QString::number(n + 1);
        QSizeF sz = calculateBlockSize(label, true);
        GraphBlock b{QString("output_ch%1").arg(n), label, 0, y, sz.width(), sz.height(), true, totalLength};
        m_blocks.push_back(b);
        playBoxBlocks.push_back(b);

        if (!stages.empty() && n < static_cast<int>(stages.back().size()) && !stages.back()[n].empty()) {
            GraphBlock srcBlock = stages.back()[n].back();
            m_arrows.push_back(GraphArrow{QString("arrow_play_%1").arg(n), srcBlock.id, b.id, QPointF(0, srcBlock.y),
                                          QPointF(0, b.y), ""});
        }
    }

    QString playName = QString::fromStdString(settings->deviceConfig.playback.deviceName().value_or("Playback Output"));
    m_boxes.push_back(makeContainerBox("box_output", playName, totalLength, playBoxBlocks));

    // Layout resolution: calculate column widths and X positions
    std::vector<qreal> columnWidths(totalLength + 1, 0.0);
    for (const auto& b : m_blocks) {
        if (b.stepIndex >= 0 && b.stepIndex <= totalLength) {
            columnWidths[b.stepIndex] = std::max(columnWidths[b.stepIndex], b.width);
        }
    }
    for (const auto& box : m_boxes) {
        if (box.stepIndex >= 0 && box.stepIndex <= totalLength) {
            columnWidths[box.stepIndex] = std::max(columnWidths[box.stepIndex], box.width);
        }
    }

    std::vector<qreal> xPositions(totalLength + 1, 0.0);
    xPositions[0] = 0.0;
    for (int s = 1; s <= totalLength; ++s) {
        qreal prevHalf = columnWidths[s - 1] / 2.0;
        qreal currHalf = columnWidths[s] / 2.0;
        qreal minSpacing = m_xStep;
        qreal neededSpacing = prevHalf + 48.0 + currHalf;
        xPositions[s] = xPositions[s - 1] + std::max(minSpacing, neededSpacing);
    }

    for (auto& b : m_blocks) {
        if (b.stepIndex >= 0 && b.stepIndex <= totalLength) {
            b.x = xPositions[b.stepIndex];
        }
    }

    for (auto& box : m_boxes) {
        if (box.stepIndex >= 0 && box.stepIndex <= totalLength) {
            box.centerX = xPositions[box.stepIndex];
        }
    }

    for (const auto& b : m_blocks) {
        m_blocksMap[b.id] = b;
    }

    for (auto& arrow : m_arrows) {
        auto srcIt = m_blocksMap.find(arrow.fromBlockId);
        auto destIt = m_blocksMap.find(arrow.toBlockId);
        if (srcIt != m_blocksMap.end()) {
            arrow.fromFallback = QPointF(srcIt->second.x + srcIt->second.width / 2.0, srcIt->second.y);
        }
        if (destIt != m_blocksMap.end()) {
            arrow.toFallback = QPointF(destIt->second.x - destIt->second.width / 2.0, destIt->second.y);
        }
    }

    qreal lastX =
        (totalLength >= 0 && totalLength < static_cast<int>(xPositions.size())) ? xPositions[totalLength] : 0.0;
    qreal lastColW =
        (totalLength >= 0 && totalLength < static_cast<int>(columnWidths.size())) ? columnWidths[totalLength] : 76.0;
    qreal totalWidth = lastX + lastColW / 2.0 + m_canvasPadding * 2 + 40;
    qreal totalHeight = (maxY - minY) + m_canvasPadding * 2 + m_titleHeaderHeight + 40;

    // Check custom position overrides to adjust canvas bounds (matching SwiftUI dynamicCanvasSize)
    qreal originY = totalHeight / 2.0 + m_titleHeaderHeight / 2.0;
    qreal maxCanvasX = totalWidth - m_canvasPadding - 60;
    qreal maxCanvasY = totalHeight - m_canvasPadding - 40;

    for (const auto& b : m_blocks) {
        QPointF pos = getBlockPos(b, originY);
        maxCanvasX = std::max(maxCanvasX, pos.x() + b.width / 2.0 + 60);
        maxCanvasY = std::max(maxCanvasY, pos.y() + b.height / 2.0 + 40);
    }

    qreal calculatedW = std::max(totalWidth, maxCanvasX + m_canvasPadding);
    qreal calculatedH = std::max(totalHeight, maxCanvasY + m_canvasPadding);

    setFixedSize(static_cast<int>(std::ceil(calculatedW)), static_cast<int>(std::ceil(calculatedH)));
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
            minX -= 14.0;
            maxX += 14.0;
            minY -= 12.0;
            maxY += 12.0;

            boxCenterX = (minX + maxX) / 2.0;
            boxCenterY = (minY + maxY) / 2.0;
            boxW = std::max(box.width, maxX - minX);
            boxH = std::max(40.0, maxY - minY);
        }

        QRectF boxRect(boxCenterX - boxW / 2.0, boxCenterY - boxH / 2.0, boxW, boxH);

        QPen boxPen(palette().color(QPalette::Mid), 1, Qt::CustomDashLine);
        boxPen.setDashPattern({4, 3});
        painter.setPen(boxPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(boxRect, 10, 10);

        // Stage Title Header above Box
        QFont boldMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        boldMono.setBold(true);
        painter.setFont(boldMono);
        painter.setPen(palette().color(QPalette::Highlight));
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

        painter.setPen(QPen(QColor(140, 140, 140, 153), 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);

        // Arrowhead cap
        QPolygonF capPolygon;
        capPolygon << p1 << QPointF(p1.x() - 6, p1.y() - 3.5) << QPointF(p1.x() - 6, p1.y() + 3.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(140, 140, 140, 204));
        painter.drawPolygon(capPolygon);

        // Arrow text label
        if (!arrow.label.isEmpty()) {
            QPointF midPoint(p0.x() + dx * 0.65, p0.y() + (p1.y() - p0.y()) * 0.65);
            QFont demiMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            demiMono.setPointSize(8);
            painter.setFont(demiMono);
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(midPoint.x() - 35, midPoint.y() - 14, 70, 28), Qt::AlignCenter, arrow.label);
        }
    }

    // 3. Render Draggable Interactive Blocks (Layer 3)
    for (const auto& b : m_blocks) {
        QPointF pos = getBlockPos(b, originY);
        QRectF bRect(pos.x() - b.width / 2.0, pos.y() - b.height / 2.0, b.width, b.height);

        if (b.isChannelPort) {
            painter.setBrush(palette().color(QPalette::Highlight).lighter(180));
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1));
            painter.drawRoundedRect(bRect, 6, 6);

            QFont boldMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            boldMono.setBold(true);
            painter.setFont(boldMono);
            painter.setPen(palette().color(QPalette::Highlight));
            painter.drawText(bRect, Qt::AlignCenter, b.label);
        } else if (!b.label.isEmpty()) {
            painter.setBrush(palette().color(QPalette::Base));
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1));
            painter.drawRoundedRect(bRect, 6, 6);

            QFont demiMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            painter.setFont(demiMono);
            painter.setPen(palette().color(QPalette::Text));
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
    : QGroupBox("", parent), m_dspController(dspController) {
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
    auto titleLbl = new QLabel("DSP Signal Processing Graph", this);
    QFont titleFont = font();
    titleFont.setPointSize(13);
    titleFont.setBold(true);
    titleLbl->setFont(titleFont);
    topRow->addWidget(titleLbl);

    m_resetLayoutBtn = new QPushButton("Reset Layout", this);
    m_resetLayoutBtn->setVisible(false);
    topRow->addWidget(m_resetLayoutBtn);
    topRow->addStretch();

    titleVBox->addLayout(topRow);
    headerBox->addLayout(titleVBox, 1);

    m_activeStagesBadge = new QLabel(this);
    headerBox->addWidget(m_activeStagesBadge, 0, Qt::AlignVCenter);

    rootLayout->addLayout(headerBox);

    // Scrollable 2D Canvas Area
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_canvas = new DSPGraphCanvas(m_dspController, m_scrollArea);
    connect(m_canvas, &DSPGraphCanvas::layoutChanged, [this]() {
        if (m_resetLayoutBtn)
            m_resetLayoutBtn->setVisible(m_canvas->hasCustomPositions());
        updateScrollHeight();
    });

    connect(m_resetLayoutBtn, &QPushButton::clicked, [this]() {
        if (m_canvas)
            m_canvas->resetLayout();
        m_resetLayoutBtn->setVisible(false);
    });

    m_scrollArea->setWidget(m_canvas);
    rootLayout->addWidget(m_scrollArea);
}

void DSPDetailedSignalGraphCard::updateScrollHeight() {
    if (m_scrollArea && m_canvas) {
        int sbH = (m_scrollArea->horizontalScrollBar() && m_scrollArea->horizontalScrollBar()->isVisible())
                      ? m_scrollArea->horizontalScrollBar()->height()
                      : 16;
        m_scrollArea->setFixedHeight(m_canvas->height() + sbH + 8);
    }
}

void DSPDetailedSignalGraphCard::showEvent(QShowEvent* event) {
    QGroupBox::showEvent(event);
    updateScrollHeight();
    QTimer::singleShot(0, this, &DSPDetailedSignalGraphCard::updateScrollHeight);
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
        updateScrollHeight();
        QTimer::singleShot(0, this, &DSPDetailedSignalGraphCard::updateScrollHeight);
    }
}
