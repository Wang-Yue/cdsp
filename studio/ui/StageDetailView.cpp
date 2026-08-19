#include "ui/StageDetailView.h"

#include <QButtonGroup>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

VSliderWidget::VSliderWidget(double value, double minVal, double maxVal, QWidget* parent)
    : QWidget(parent), m_value(value), m_minVal(minVal), m_maxVal(maxVal) {
    setFixedWidth(36);
    setFixedHeight(160);
}

void VSliderWidget::setValue(double val) {
    double clamped = std::clamp(val, m_minVal, m_maxVal);
    if (std::abs(m_value - clamped) > 1e-4) {
        m_value = clamped;
        update();
        emit valueChanged(m_value);
    }
}

void VSliderWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();
    int trackW = 4;
    int knobR = 7;
    int centerX = w / 2;

    int topY = knobR;
    int botY = h - knobR;
    int trackHeight = botY - topY;

    // Track background
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::Mid));
    painter.drawRoundedRect(centerX - trackW / 2, topY, trackW, trackHeight, trackW / 2, trackW / 2);

    // Current value knob Y
    double centerPct = (0.0 - m_minVal) / (m_maxVal - m_minVal);
    int centerY = botY - static_cast<int>(centerPct * trackHeight);
    double valPct = (m_value - m_minVal) / (m_maxVal - m_minVal);
    int knobY = botY - static_cast<int>(valPct * trackHeight);

    // Active fill track growing from 0 dB center
    int activeTop = std::min(knobY, centerY);
    int activeHeight = std::abs(knobY - centerY);

    if (activeHeight > 0) {
        painter.setBrush(palette().color(QPalette::Highlight));
        painter.drawRoundedRect(centerX - trackW / 2, activeTop, trackW, activeHeight, trackW / 2, trackW / 2);
    }

    // Center 0 dB tick line (drawn over active fill track for visibility)
    painter.setPen(QPen(palette().color(QPalette::PlaceholderText), 1));
    painter.drawLine(centerX - 6, centerY, centerX + 6, centerY);

    // Knob Circle
    painter.setBrush(palette().color(QPalette::Base));
    painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
    painter.drawEllipse(QPoint(centerX, knobY), knobR, knobR);
}

void VSliderWidget::mousePressEvent(QMouseEvent* event) {
    updateValueFromMouse(event->pos().y());
}

void VSliderWidget::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        updateValueFromMouse(event->pos().y());
    }
}

void VSliderWidget::updateValueFromMouse(int y) {
    int knobR = 7;
    int topY = knobR;
    int botY = height() - knobR;
    int trackHeight = botY - topY;

    int clampedY = std::clamp(y, topY, botY);
    double pct = static_cast<double>(botY - clampedY) / trackHeight;
    double newVal = m_minVal + pct * (m_maxVal - m_minVal);
    setValue(newVal);
}

RotatedLabel::RotatedLabel(const QString& text, QWidget* parent) : QWidget(parent), m_text(text) {
    setFixedSize(35, 30);
}

void RotatedLabel::setText(const QString& text) {
    m_text = text;
    update();
}

void RotatedLabel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont f = font();
    f.setBold(true);
    painter.setFont(f);
    painter.setPen(palette().color(QPalette::Text));

    painter.translate(width() / 2.0, height() / 2.0);
    painter.rotate(-90);

    QRectF rect(-height() / 2.0, -width() / 2.0, height(), width());
    painter.drawText(rect, Qt::AlignCenter, m_text);
}

QSize RotatedLabel::sizeHint() const {
    return QSize(35, 30);
}

namespace {

QWidget* createSliderField(QSlider* slider, QLabel* valueLabel, QWidget* extraWidget = nullptr) {
    auto container = new QWidget();
    auto layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    layout->addWidget(slider);

    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    valueLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    valueLabel->setMinimumWidth(65);
    layout->addWidget(valueLabel);

    if (extraWidget) {
        layout->addWidget(extraWidget);
    }

    return container;
}

void addSliderRow(QFormLayout* form, const QString& labelText, QSlider* slider, QLabel* valueLabel, QWidget* parent,
                  QWidget* extraWidget = nullptr) {
    auto label = new QLabel(labelText, parent);
    label->setBuddy(slider);
    form->addRow(label, createSliderField(slider, valueLabel, extraWidget));
}

void addWidgetRow(QFormLayout* form, const QString& labelText, QWidget* fieldWidget, QWidget* parent) {
    auto label = new QLabel(labelText, parent);
    label->setBuddy(fieldWidget);
    form->addRow(label, fieldWidget);
}

} // namespace

QWidget* StageDetailView::createMatrixCellWidget(PipelineStage& stage, int dest, int src, QTableWidget* table) {
    auto cellWidget = new QWidget(table);
    cellWidget->setFixedSize(120, 95);

    auto mapIt = std::find_if(stage.mixerMappings.begin(), stage.mixerMappings.end(),
                              [dest](const MixerMapping& m) { return m.dest == dest; });
    const MixerSource* srcPtr = nullptr;
    if (mapIt != stage.mixerMappings.end()) {
        auto srcIt = std::find_if(mapIt->sources.begin(), mapIt->sources.end(),
                                  [src](const MixerSource& s) { return s.channel == src; });
        if (srcIt != mapIt->sources.end()) {
            srcPtr = &(*srcIt);
        }
    }

    bool isConnected = (srcPtr != nullptr);

    if (isConnected) {
        auto vBox = new QVBoxLayout(cellWidget);
        vBox->setContentsMargins(4, 4, 4, 4);
        vBox->setSpacing(4);
        vBox->setAlignment(Qt::AlignCenter);

        // Top Checkmark button
        auto checkBtn = new QPushButton("☑", cellWidget);
        checkBtn->setFixedSize(30, 22);
        QFont f = checkBtn->font();
        f.setPointSize(13);
        checkBtn->setFont(f);
        checkBtn->setFlat(true);
        checkBtn->setCursor(Qt::PointingHandCursor);
        checkBtn->setToolTip("Disconnect Source");
        connect(checkBtn, &QPushButton::clicked, [this, dest, src, table]() {
            auto st = currentStage();
            if (!st)
                return;
            auto mapIt = std::find_if(st->mixerMappings.begin(), st->mixerMappings.end(),
                                      [dest](const MixerMapping& m) { return m.dest == dest; });
            if (mapIt != st->mixerMappings.end()) {
                mapIt->sources.erase(std::remove_if(mapIt->sources.begin(), mapIt->sources.end(),
                                                    [src](const MixerSource& s) { return s.channel == src; }),
                                     mapIt->sources.end());
            }
            applyConfig();
            table->setCellWidget(dest, src, createMatrixCellWidget(*st, dest, src, table));
        });
        vBox->addWidget(checkBtn, 0, Qt::AlignCenter);

        // Middle Gain line edit
        auto gainEdit = new QLineEdit(cellWidget);
        gainEdit->setFixedWidth(70);
        gainEdit->setFixedHeight(22);
        gainEdit->setAlignment(Qt::AlignCenter);
        gainEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        gainEdit->setText(QString::number(srcPtr->gainValue(), 'f', 1));
        connect(gainEdit, &QLineEdit::editingFinished, [this, dest, src, gainEdit]() {
            bool ok = false;
            double val = gainEdit->text().toDouble(&ok);
            if (ok) {
                auto st = currentStage();
                if (!st)
                    return;
                auto mapIt = std::find_if(st->mixerMappings.begin(), st->mixerMappings.end(),
                                          [dest](const MixerMapping& m) { return m.dest == dest; });
                if (mapIt != st->mixerMappings.end()) {
                    for (auto& s : mapIt->sources) {
                        if (s.channel == src) {
                            s.gain = val;
                            break;
                        }
                    }
                    applyConfig();
                }
            }
        });
        vBox->addWidget(gainEdit, 0, Qt::AlignCenter);

        // Bottom 3 Action Buttons (Phase, Mute, Scale)
        auto btnHBox = new QHBoxLayout();
        btnHBox->setSpacing(6);
        btnHBox->setContentsMargins(0, 0, 0, 0);
        btnHBox->setAlignment(Qt::AlignCenter);

        bool inv = srcPtr->inverted.value_or(false);
        auto invBtn = new QPushButton(inv ? "Ø" : "+", cellWidget);
        invBtn->setFixedSize(30, 24);
        invBtn->setFlat(true);
        invBtn->setToolTip("Invert Phase");
        connect(invBtn, &QPushButton::clicked, [this, dest, src, table]() {
            auto st = currentStage();
            if (!st)
                return;
            auto mapIt = std::find_if(st->mixerMappings.begin(), st->mixerMappings.end(),
                                      [dest](const MixerMapping& m) { return m.dest == dest; });
            if (mapIt != st->mixerMappings.end()) {
                for (auto& s : mapIt->sources) {
                    if (s.channel == src) {
                        s.inverted = !s.inverted.value_or(false);
                        break;
                    }
                }
                applyConfig();
                table->setCellWidget(dest, src, createMatrixCellWidget(*st, dest, src, table));
            }
        });
        btnHBox->addWidget(invBtn);

        bool muted = srcPtr->mute.value_or(false);
        auto muteBtn = new QPushButton(cellWidget);
        muteBtn->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
        muteBtn->setFixedSize(30, 24);
        muteBtn->setFlat(true);
        muteBtn->setToolTip("Mute Source");
        connect(muteBtn, &QPushButton::clicked, [this, dest, src, table]() {
            auto st = currentStage();
            if (!st)
                return;
            auto mapIt = std::find_if(st->mixerMappings.begin(), st->mixerMappings.end(),
                                      [dest](const MixerMapping& m) { return m.dest == dest; });
            if (mapIt != st->mixerMappings.end()) {
                for (auto& s : mapIt->sources) {
                    if (s.channel == src) {
                        s.mute = !s.mute.value_or(false);
                        break;
                    }
                }
                applyConfig();
                table->setCellWidget(dest, src, createMatrixCellWidget(*st, dest, src, table));
            }
        });
        btnHBox->addWidget(muteBtn);

        GainScale sc = srcPtr->scale.value_or(GainScale::dB);
        auto scaleBtn = new QPushButton(sc == GainScale::dB ? "dB" : "lin", cellWidget);
        scaleBtn->setFixedSize(30, 24);
        scaleBtn->setFlat(true);
        scaleBtn->setToolTip("Toggle Gain Scale (dB / Linear)");
        connect(scaleBtn, &QPushButton::clicked, [this, dest, src, table]() {
            auto st = currentStage();
            if (!st)
                return;
            auto mapIt = std::find_if(st->mixerMappings.begin(), st->mixerMappings.end(),
                                      [dest](const MixerMapping& m) { return m.dest == dest; });
            if (mapIt != st->mixerMappings.end()) {
                for (auto& s : mapIt->sources) {
                    if (s.channel == src) {
                        s.scale =
                            (s.scale.value_or(GainScale::dB) == GainScale::dB) ? GainScale::linear : GainScale::dB;
                        break;
                    }
                }
                applyConfig();
                table->setCellWidget(dest, src, createMatrixCellWidget(*st, dest, src, table));
            }
        });
        btnHBox->addWidget(scaleBtn);

        vBox->addLayout(btnHBox);
    } else {
        auto vBox = new QVBoxLayout(cellWidget);
        vBox->setContentsMargins(6, 6, 6, 6);

        auto checkBtn = new QPushButton("☐", cellWidget);
        QFont f = checkBtn->font();
        f.setPointSize(16);
        checkBtn->setFont(f);
        checkBtn->setFlat(true);
        checkBtn->setCursor(Qt::PointingHandCursor);
        checkBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        checkBtn->setToolTip("Click to connect source");
        connect(checkBtn, &QPushButton::clicked, [this, dest, src, table]() {
            auto st = currentStage();
            if (!st)
                return;
            auto mapIt = std::find_if(st->mixerMappings.begin(), st->mixerMappings.end(),
                                      [dest](const MixerMapping& m) { return m.dest == dest; });
            if (mapIt == st->mixerMappings.end()) {
                st->mixerMappings.push_back(MixerMapping{dest, {}, std::nullopt});
                mapIt = std::prev(st->mixerMappings.end());
            }
            if (std::find_if(mapIt->sources.begin(), mapIt->sources.end(),
                             [src](const MixerSource& s) { return s.channel == src; }) == mapIt->sources.end()) {
                mapIt->sources.push_back(MixerSource{src, 0.0, std::nullopt, std::nullopt, std::nullopt});
            }
            applyConfig();
            table->setCellWidget(dest, src, createMatrixCellWidget(*st, dest, src, table));
        });
        vBox->addWidget(checkBtn);
    }

    return cellWidget;
}

StageDetailView::StageDetailView(size_t stageIndex, std::shared_ptr<PipelineStore> pipeline,
                                 std::shared_ptr<DSPEngineController> dspController, QWidget* parent)
    : QWidget(parent), m_stageIndex(stageIndex), m_pipeline(pipeline), m_dspController(dspController) {
    setupUi();
    if (m_pipeline) {
        connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, &StageDetailView::refreshUi);
    }
    refreshUi();
}

void StageDetailView::setupUi() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto container = new QWidget(scroll);
    auto mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    // Header Toolbar
    auto headerLayout = new QHBoxLayout();
    m_nameEdit = new QLineEdit(container);
    QFont nameFont = font();
    nameFont.setPointSize(14);
    nameFont.setBold(true);
    m_nameEdit->setFont(nameFont);
    connect(m_nameEdit, &QLineEdit::editingFinished, [this]() {
        if (m_pipeline && m_stageIndex < m_pipeline->stages.size()) {
            m_pipeline->stages[m_stageIndex].name = m_nameEdit->text().toStdString();
            applyConfig();
        }
    });
    headerLayout->addWidget(m_nameEdit);

    headerLayout->addStretch();

    m_enabledCheck = new QCheckBox("Enabled", container);
    connect(m_enabledCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_pipeline && m_stageIndex < m_pipeline->stages.size()) {
            m_pipeline->stages[m_stageIndex].isEnabled = checked;
            if (m_optionsContainer) {
                m_optionsContainer->setEnabled(checked);
            }
            applyConfig();
        }
    });
    headerLayout->addWidget(m_enabledCheck);

    mainLayout->addLayout(headerLayout);

    // Options Container
    m_optionsContainer = new QWidget(container);
    m_optionsContainer->setLayout(new QVBoxLayout());
    m_optionsContainer->layout()->setContentsMargins(0, 0, 0, 0);

    mainLayout->addWidget(m_optionsContainer);
    mainLayout->addStretch();

    scroll->setWidget(container);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void StageDetailView::applyConfig() {
    if (m_isBuildingUi || !m_pipeline)
        return;
    m_isLocalEditing = true;
    m_pipeline->save();
    emit m_pipeline->pipelineChanged();
    m_isLocalEditing = false;
}

PipelineStage* StageDetailView::currentStage() const {
    if (m_pipeline && m_stageIndex < m_pipeline->stages.size()) {
        return &m_pipeline->stages[m_stageIndex];
    }
    return nullptr;
}

void StageDetailView::refreshUi() {
    if (m_isLocalEditing || !m_pipeline)
        return;
    m_isBuildingUi = true;
    if (m_stageIndex < m_pipeline->stages.size()) {
        const auto& stage = m_pipeline->stages[m_stageIndex];

        m_nameEdit->blockSignals(true);
        m_enabledCheck->blockSignals(true);

        m_nameEdit->setText(QString::fromStdString(stage.name));
        m_enabledCheck->setChecked(stage.isEnabled);

        m_nameEdit->blockSignals(false);
        m_enabledCheck->blockSignals(false);

        buildStageOptionsUi();
    }
    m_isBuildingUi = false;
}

QWidget* StageDetailView::createChannelSelectorWidget(int incomingChannels,
                                                      const std::function<std::vector<int>()>& getter,
                                                      const std::function<void(const std::vector<int>&)>& setter,
                                                      QWidget* parent) {
    auto scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(32);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }\n"
                          "QScrollArea > QWidget > QWidget { background: transparent; border: none; }\n"
                          "QWidget#ChannelPillsViewport { background: transparent; border: none; }\n"
                          "QWidget#ChannelPillsContainer { background: transparent; border: none; }");
    scroll->viewport()->setObjectName("ChannelPillsViewport");
    scroll->viewport()->setAutoFillBackground(false);
    scroll->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);

    auto container = new QWidget(scroll);
    container->setObjectName("ChannelPillsContainer");
    container->setAutoFillBackground(false);
    container->setAttribute(Qt::WA_TranslucentBackground, true);
    auto layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    std::vector<int> initialSelected = getter();
    for (int c = 0; c < incomingChannels; ++c) {
        auto btn = new QPushButton(QString::number(c + 1), container);
        int btnWidth = (c + 1 >= 100) ? 46 : ((c + 1 >= 10) ? 40 : 36);
        btn->setFixedWidth(btnWidth);
        btn->setCheckable(true);
        bool isSelected = std::find(initialSelected.begin(), initialSelected.end(), c) != initialSelected.end();
        btn->setChecked(isSelected);

        connect(btn, &QPushButton::clicked, [c, btn, getter, setter]() {
            std::vector<int> channels = getter();
            auto it = std::find(channels.begin(), channels.end(), c);
            if (it != channels.end()) {
                if (channels.size() > 1) {
                    channels.erase(it);
                    btn->setChecked(false);
                    setter(channels);
                } else {
                    btn->setChecked(true);
                }
            } else {
                channels.push_back(c);
                btn->setChecked(true);
                setter(channels);
            }
        });
        layout->addWidget(btn);
    }
    layout->addStretch();
    scroll->setWidget(container);
    return scroll;
}

void StageDetailView::buildStageOptionsUi() {
    auto containerLayout = qobject_cast<QVBoxLayout*>(m_optionsContainer->layout());
    QLayoutItem* item;
    while ((item = containerLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }

    if (m_stageIndex >= m_pipeline->stages.size())
        return;
    auto& stage = m_pipeline->stages[m_stageIndex];

    m_optionsContainer->setEnabled(stage.isEnabled);

    int hwChannels =
        (m_dspController && m_dspController->devices()) ? m_dspController->devices()->captureConfig.channels : 8;
    int incomingChannels = m_pipeline ? m_pipeline->channelCountBeforeStage(m_stageIndex, hwChannels) : hwChannels;
    if (incomingChannels < 1)
        incomingChannels = 2;

    // 1. Channel Selector Card (Unified for all stages except Matrix Mixer)
    if (stage.type != StageType::MatrixMixer) {
        auto chanGroup = new QGroupBox("Target Channels", m_optionsContainer);
        chanGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        if (stage.type == StageType::Balance || stage.type == StageType::Width || stage.type == StageType::MSProc ||
            stage.type == StageType::Crossfeed || stage.type == StageType::RACE ||
            stage.type == StageType::SplitWidth) {
            auto chanForm = new QFormLayout(chanGroup);
            chanForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

            if (stage.leftChannel >= incomingChannels) {
                stage.leftChannel = 0;
            }
            if (stage.rightChannel >= incomingChannels) {
                stage.rightChannel = std::min(1, incomingChannels - 1);
            }

            auto leftCombo = new QComboBox(chanGroup);
            for (int c = 0; c < incomingChannels; ++c) {
                leftCombo->addItem(QString("Channel %1").arg(c + 1), c);
            }
            leftCombo->setCurrentIndex(std::min(stage.leftChannel, incomingChannels - 1));
            connect(leftCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, leftCombo](int idx) {
                auto st = currentStage();
                if (!st)
                    return;
                st->leftChannel = leftCombo->itemData(idx).toInt();
                applyConfig();
            });
            addWidgetRow(chanForm, "&Left Input:", leftCombo, chanGroup);

            auto rightCombo = new QComboBox(chanGroup);
            for (int c = 0; c < incomingChannels; ++c) {
                rightCombo->addItem(QString("Channel %1").arg(c + 1), c);
            }
            rightCombo->setCurrentIndex(std::min(stage.rightChannel, incomingChannels - 1));
            connect(rightCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, rightCombo](int idx) {
                auto st = currentStage();
                if (!st)
                    return;
                st->rightChannel = rightCombo->itemData(idx).toInt();
                applyConfig();
            });
            addWidgetRow(chanForm, "&Right Input:", rightCombo, chanGroup);

            auto descLbl = new QLabel("This stereo stage will process the selected Left and Right channels. All other "
                                      "channels will pass through unaffected.",
                                      chanGroup);
            descLbl->setWordWrap(true);
            chanForm->addRow(descLbl);
        } else {
            auto chanLayout = new QVBoxLayout(chanGroup);
            auto pillsScroll = createChannelSelectorWidget(
                incomingChannels,
                [this]() -> std::vector<int> {
                    auto st = currentStage();
                    return st ? st->channels : std::vector<int>{};
                },
                [this](const std::vector<int>& chs) {
                    auto st = currentStage();
                    if (st) {
                        st->channels = chs;
                        applyConfig();
                    }
                },
                chanGroup);
            chanLayout->addWidget(pillsScroll);
        }
        containerLayout->addWidget(chanGroup);
    }

    // 2. Stage Options Panels
    switch (stage.type) {
    case StageType::Balance: {
        auto balGroup = new QGroupBox("Balance", m_optionsContainer);
        auto balForm = new QFormLayout(balGroup);
        balForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto sliderContainer = new QWidget(balGroup);
        auto sliderHBox = new QHBoxLayout(sliderContainer);
        sliderHBox->setContentsMargins(0, 0, 0, 0);
        sliderHBox->setSpacing(8);

        sliderHBox->addWidget(new QLabel("L", sliderContainer));
        auto balSlider = new QSlider(Qt::Horizontal, sliderContainer);
        balSlider->setRange(-100, 100);
        balSlider->setValue(static_cast<int>(stage.balancePosition * 100.0));
        sliderHBox->addWidget(balSlider);
        sliderHBox->addWidget(new QLabel("R", sliderContainer));

        auto centerBtn = new QPushButton("Center", sliderContainer);
        sliderHBox->addWidget(centerBtn);

        addWidgetRow(balForm, "&Balance:", sliderContainer, balGroup);

        auto infoContainer = new QWidget(balGroup);
        auto infoHBox = new QHBoxLayout(infoContainer);
        infoHBox->setContentsMargins(0, 0, 0, 0);
        infoHBox->setSpacing(16);

        auto leftLbl = new QLabel(QString("Left: %1%").arg(stage.balanceLeftPercent()), infoContainer);
        infoHBox->addWidget(leftLbl);
        infoHBox->addStretch();
        auto rightLbl = new QLabel(QString("Right: %1%").arg(stage.balanceRightPercent()), infoContainer);
        infoHBox->addWidget(rightLbl);

        balForm->addRow("Levels:", infoContainer);

        connect(centerBtn, &QPushButton::clicked, [this, balSlider]() {
            auto st = currentStage();
            if (!st)
                return;
            st->balancePosition = 0.0;
            balSlider->setValue(0);
            applyConfig();
            refreshUi();
        });

        connect(balSlider, &QSlider::valueChanged, [this, leftLbl, rightLbl](int val) {
            auto st = currentStage();
            if (!st)
                return;
            st->balancePosition = val / 100.0;
            leftLbl->setText(QString("Left: %1%").arg(st->balanceLeftPercent()));
            rightLbl->setText(QString("Right: %1%").arg(st->balanceRightPercent()));
            applyConfig();
        });

        containerLayout->addWidget(balGroup);
        break;
    }

    case StageType::Width: {
        auto widthGroup = new QGroupBox("Stereo Width", m_optionsContainer);
        auto widthForm = new QFormLayout(widthGroup);
        widthForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto widthSlider = new QSlider(Qt::Horizontal, widthGroup);
        widthSlider->setRange(-100, 200);
        widthSlider->setValue(static_cast<int>(stage.widthAmount * 100.0));

        auto pctLbl = new QLabel(QString("%1%").arg(stage.widthPercent()), widthGroup);
        QFont pctFont = font();
        pctFont.setPointSize(12);
        pctFont.setBold(true);
        pctLbl->setFont(pctFont);

        addSliderRow(widthForm, "&Width:", widthSlider, pctLbl, widthGroup);

        auto presetContainer = new QWidget(widthGroup);
        auto presetHBox = new QHBoxLayout(presetContainer);
        presetHBox->setContentsMargins(0, 0, 0, 0);
        presetHBox->setSpacing(8);

        auto btnNeg100 = new QPushButton("-100% (Swapped)", presetContainer);
        connect(btnNeg100, &QPushButton::clicked, [this, &stage]() {
            stage.widthAmount = -1.0;
            applyConfig();
            refreshUi();
        });
        presetHBox->addWidget(btnNeg100);

        auto btnMono = new QPushButton("Mono (0%)", presetContainer);
        connect(btnMono, &QPushButton::clicked, [this, &stage]() {
            stage.widthAmount = 0.0;
            applyConfig();
            refreshUi();
        });
        presetHBox->addWidget(btnMono);

        auto btn100 = new QPushButton("100% (Normal)", presetContainer);
        connect(btn100, &QPushButton::clicked, [this, &stage]() {
            stage.widthAmount = 1.0;
            applyConfig();
            refreshUi();
        });
        presetHBox->addWidget(btn100);

        presetHBox->addStretch();
        widthForm->addRow("Presets:", presetContainer);

        auto descLbl = new QLabel(QString::fromStdString(stage.widthDescription()), widthGroup);
        descLbl->setWordWrap(true);
        widthForm->addRow("Status:", descLbl);

        connect(widthSlider, &QSlider::valueChanged, [this, &stage, pctLbl, descLbl](int val) {
            stage.widthAmount = val / 100.0;
            pctLbl->setText(QString("%1%").arg(stage.widthPercent()));
            descLbl->setText(QString::fromStdString(stage.widthDescription()));
            applyConfig();
        });

        containerLayout->addWidget(widthGroup);
        break;
    }

    case StageType::MSProc: {
        auto msGroup = new QGroupBox("Mid/Side Processing", m_optionsContainer);
        auto msBox = new QVBoxLayout(msGroup);
        auto msLbl = new QLabel("Encodes stereo to Mid (L+R) and Side (L-R) signals at -6.02 dB.", msGroup);
        msBox->addWidget(msLbl);
        containerLayout->addWidget(msGroup);
        break;
    }

    case StageType::PhaseInvert: {
        auto piGroup = new QGroupBox("Phase Inversion", m_optionsContainer);
        auto piBox = new QVBoxLayout(piGroup);
        auto piLbl = new QLabel("Inverts the phase (polarity) of all selected channels.", piGroup);
        piBox->addWidget(piLbl);
        containerLayout->addWidget(piGroup);
        break;
    }

    case StageType::Crossfeed: {
        auto presetGroup = new QGroupBox("Preset", m_optionsContainer);
        auto presetForm = new QFormLayout(presetGroup);
        presetForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        presetGroup->setEnabled(!stage.cxCustomEnabled);

        auto levelCombo = new QComboBox(presetGroup);
        levelCombo->addItems({"L1", "L2", "L3", "L4", "L5"});
        levelCombo->setCurrentIndex(static_cast<int>(stage.crossfeedLevel) - 1);
        connect(levelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
            auto st = currentStage();
            if (!st)
                return;
            st->crossfeedLevel = static_cast<CrossfeedLevel>(idx + 1);
            applyConfig();
            refreshUi();
        });
        addWidgetRow(presetForm, "&Level:", levelCombo, presetGroup);

        double presetFc = 700.0;
        double presetDb = 6.0;
        switch (stage.crossfeedLevel) {
        case CrossfeedLevel::L1:
            presetFc = 650.0;
            presetDb = 13.5;
            break;
        case CrossfeedLevel::L2:
            presetFc = 650.0;
            presetDb = 9.5;
            break;
        case CrossfeedLevel::L3:
            presetFc = 700.0;
            presetDb = 6.0;
            break;
        case CrossfeedLevel::L4:
            presetFc = 700.0;
            presetDb = 4.5;
            break;
        case CrossfeedLevel::L5:
            presetFc = 700.0;
            presetDb = 3.0;
            break;
        case CrossfeedLevel::Off:
            break;
        }

        auto detailLbl = new QLabel(QString("Fc = %1 Hz, Level = %2 dB — %3")
                                        .arg(presetFc, 0, 'f', 0)
                                        .arg(presetDb, 0, 'f', 1)
                                        .arg(QString::fromStdString(crossfeedLevelDescription(stage.crossfeedLevel))),
                                    presetGroup);
        detailLbl->setWordWrap(true);
        presetForm->addRow("Details:", detailLbl);

        containerLayout->addWidget(presetGroup);

        auto customGroup = new QGroupBox("Custom Parameters", m_optionsContainer);
        auto customForm = new QFormLayout(customGroup);
        customForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto customToggle = new QCheckBox("Enable Custom Parameters", customGroup);
        customToggle->setChecked(stage.cxCustomEnabled);
        connect(customToggle, &QCheckBox::toggled, [this](bool checked) {
            auto st = currentStage();
            if (!st)
                return;
            st->cxCustomEnabled = checked;
            if (checked) {
                double fc = 700.0;
                double db = 6.0;
                switch (st->crossfeedLevel) {
                case CrossfeedLevel::L1:
                    fc = 650.0;
                    db = 13.5;
                    break;
                case CrossfeedLevel::L2:
                    fc = 650.0;
                    db = 9.5;
                    break;
                case CrossfeedLevel::L3:
                    fc = 700.0;
                    db = 6.0;
                    break;
                case CrossfeedLevel::L4:
                    fc = 700.0;
                    db = 4.5;
                    break;
                case CrossfeedLevel::L5:
                    fc = 700.0;
                    db = 3.0;
                    break;
                case CrossfeedLevel::Off:
                    break;
                }
                st->cxFc = fc;
                st->cxDb = db;
            }
            applyConfig();
            refreshUi();
        });
        customForm->addRow("", customToggle);

        if (stage.cxCustomEnabled) {
            auto fcSlider = new QSlider(Qt::Horizontal, customGroup);
            fcSlider->setRange(300, 1200);
            fcSlider->setSingleStep(10);
            fcSlider->setPageStep(10);
            fcSlider->setValue(static_cast<int>(stage.cxFc));
            auto fcLbl = new QLabel(QString("%1 Hz").arg(static_cast<int>(stage.cxFc)), customGroup);
            connect(fcSlider, &QSlider::valueChanged, [this, fcLbl](int val) {
                auto st = currentStage();
                if (!st)
                    return;
                int rounded = (val / 10) * 10;
                st->cxFc = rounded;
                fcLbl->setText(QString("%1 Hz").arg(rounded));
                applyConfig();
            });
            addSliderRow(customForm, "Cutoff &Freq (Fc):", fcSlider, fcLbl, customGroup);

            auto dbSlider = new QSlider(Qt::Horizontal, customGroup);
            dbSlider->setRange(10, 200);
            dbSlider->setSingleStep(5);
            dbSlider->setPageStep(5);
            dbSlider->setValue(static_cast<int>(stage.cxDb * 10.0));
            auto dbLbl = new QLabel(QString("%1 dB").arg(stage.cxDb, 0, 'f', 1), customGroup);
            connect(dbSlider, &QSlider::valueChanged, [this, dbLbl](int val) {
                auto st = currentStage();
                if (!st)
                    return;
                st->cxDb = val / 10.0;
                dbLbl->setText(QString("%1 dB").arg(st->cxDb, 0, 'f', 1));
                applyConfig();
            });
            addSliderRow(customForm, "&Crossfeed Level:", dbSlider, dbLbl, customGroup);
        }
        containerLayout->addWidget(customGroup);

        auto cxParams = stage.activeCrossfeedParams();
        auto derivedGroup = new QGroupBox("Computed Filter Parameters", m_optionsContainer);
        auto derivedGrid = new QGridLayout(derivedGroup);
        derivedGrid->addWidget(new QLabel("Lowshelf", derivedGroup), 0, 0);
        derivedGrid->addWidget(new QLabel(QString("%1 Hz").arg(cxParams.hiFreq, 0, 'f', 1), derivedGroup), 0, 1);
        derivedGrid->addWidget(new QLabel(QString("%1 dB").arg(cxParams.hiGain, 0, 'f', 2), derivedGroup), 0, 2);
        derivedGrid->addWidget(new QLabel("Q 0.5", derivedGroup), 0, 3);

        derivedGrid->addWidget(new QLabel("Lowpass", derivedGroup), 1, 0);
        derivedGrid->addWidget(new QLabel(QString("%1 Hz").arg(cxParams.loFreq, 0, 'f', 0), derivedGroup), 1, 1);
        derivedGrid->addWidget(new QLabel("1st order", derivedGroup), 1, 2);

        derivedGrid->addWidget(new QLabel("Cross gain", derivedGroup), 2, 0);
        derivedGrid->addWidget(new QLabel(QString("%1 dB").arg(cxParams.loGain, 0, 'f', 2), derivedGroup), 2, 1);

        containerLayout->addWidget(derivedGroup);
        break;
    }

    case StageType::SplitWidth: {
        auto crossGroup = new QGroupBox("Crossover Frequency", m_optionsContainer);
        auto crossForm = new QFormLayout(crossGroup);
        crossForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto crossSlider = new QSlider(Qt::Horizontal, crossGroup);
        crossSlider->setRange(40, 1000);
        crossSlider->setSingleStep(5);
        crossSlider->setValue(static_cast<int>(stage.splitWidthCrossover));

        auto crossLbl = new QLabel(QString("%1 Hz").arg(static_cast<int>(stage.splitWidthCrossover)), crossGroup);
        QFont crossFont = font();
        crossFont.setBold(true);
        crossLbl->setFont(crossFont);

        addSliderRow(crossForm, "&Crossover:", crossSlider, crossLbl, crossGroup);

        auto crossDesc = new QLabel("Frequencies below this crossover will remain centered (mono-summed), while "
                                    "frequencies above this point will be widened.",
                                    crossGroup);
        crossDesc->setWordWrap(true);
        crossForm->addRow("Description:", crossDesc);

        connect(crossSlider, &QSlider::valueChanged, [this, &stage, crossLbl](int val) {
            stage.splitWidthCrossover = val;
            crossLbl->setText(QString("%1 Hz").arg(val));
            applyConfig();
        });
        containerLayout->addWidget(crossGroup);

        auto highGroup = new QGroupBox("High Band Stereo Width", m_optionsContainer);
        auto highForm = new QFormLayout(highGroup);
        highForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto highSlider = new QSlider(Qt::Horizontal, highGroup);
        highSlider->setRange(0, 200);
        highSlider->setValue(static_cast<int>(stage.splitWidthAmount * 100.0));

        auto highPctLbl = new QLabel(QString("%1%").arg(static_cast<int>(stage.splitWidthAmount * 100.0)), highGroup);
        QFont highFont = font();
        highFont.setPointSize(12);
        highFont.setBold(true);
        highPctLbl->setFont(highFont);

        addSliderRow(highForm, "&Width:", highSlider, highPctLbl, highGroup);

        auto presetContainer = new QWidget(highGroup);
        auto presetHBox = new QHBoxLayout(presetContainer);
        presetHBox->setContentsMargins(0, 0, 0, 0);
        presetHBox->setSpacing(8);

        auto btnMono = new QPushButton("Mono (0%)", presetContainer);
        connect(btnMono, &QPushButton::clicked, [this, &stage]() {
            stage.splitWidthAmount = 0.0;
            applyConfig();
            refreshUi();
        });
        presetHBox->addWidget(btnMono);

        auto btn100 = new QPushButton("100% (Normal)", presetContainer);
        connect(btn100, &QPushButton::clicked, [this, &stage]() {
            stage.splitWidthAmount = 1.0;
            applyConfig();
            refreshUi();
        });
        presetHBox->addWidget(btn100);

        auto btn150 = new QPushButton("150% (Wide)", presetContainer);
        connect(btn150, &QPushButton::clicked, [this, &stage]() {
            stage.splitWidthAmount = 1.5;
            applyConfig();
            refreshUi();
        });
        presetHBox->addWidget(btn150);

        presetHBox->addStretch();
        highForm->addRow("Presets:", presetContainer);

        auto highDesc = new QLabel("Adjusts the stereo width of frequencies above the crossover point. 0% is full "
                                   "mono, 100% is normal stereo, and 150%+ is enhanced width.",
                                   highGroup);
        highDesc->setWordWrap(true);
        highForm->addRow("Description:", highDesc);

        connect(highSlider, &QSlider::valueChanged, [this, &stage, highPctLbl](int val) {
            stage.splitWidthAmount = val / 100.0;
            highPctLbl->setText(QString("%1%").arg(val));
            applyConfig();
        });
        containerLayout->addWidget(highGroup);
        break;
    }

    case StageType::EQ: {
        if (m_pipeline->eqPresets.empty()) {
            auto noEqLbl = new QLabel("No EQ presets yet. Create one in the sidebar.", m_optionsContainer);
            containerLayout->addWidget(noEqLbl);
        } else {
            auto eqGroup = new QGroupBox("EQ Preset", m_optionsContainer);
            auto eqForm = new QFormLayout(eqGroup);
            eqForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

            auto eqCombo = new QComboBox(eqGroup);
            eqCombo->addItem("None", QString());
            for (const auto& preset : m_pipeline->eqPresets) {
                eqCombo->addItem(QString::fromStdString(preset.name), preset.id.toString());
            }
            if (stage.eqPresetId.has_value()) {
                int idx = eqCombo->findData(stage.eqPresetId->toString());
                if (idx >= 0)
                    eqCombo->setCurrentIndex(idx);
            }
            connect(eqCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, eqCombo]() {
                QString idStr = eqCombo->currentData().toString();
                if (idStr.isEmpty())
                    stage.eqPresetId = std::nullopt;
                else
                    stage.eqPresetId = QUuid::fromString(idStr);
                applyConfig();
                refreshUi();
            });
            addWidgetRow(eqForm, "&Preset:", eqCombo, eqGroup);

            if (stage.eqPresetId.has_value()) {
                auto it = std::find_if(m_pipeline->eqPresets.begin(), m_pipeline->eqPresets.end(),
                                       [&stage](const EQPreset& p) { return p.id == stage.eqPresetId.value(); });
                if (it != m_pipeline->eqPresets.end()) {
                    auto preampLbl = new QLabel(
                        QString("%1%2 dB").arg(it->preampGain >= 0 ? "+" : "").arg(it->preampGain, 0, 'f', 1), eqGroup);
                    eqForm->addRow("Preamp Gain:", preampLbl);

                    auto diagWidget = new EQDiagramWidget(eqGroup);
                    diagWidget->setPreset(*it);
                    diagWidget->setFixedHeight(150);
                    eqForm->addRow("Response Curve:", diagWidget);
                }
            }
            containerLayout->addWidget(eqGroup);
        }
        break;
    }

    case StageType::Convolution: {
        if (m_pipeline->convPresets.empty()) {
            auto noConvLbl = new QLabel(
                "No convolution presets yet. Open Room Correction → Generate FIR to create one.", m_optionsContainer);
            containerLayout->addWidget(noConvLbl);
        } else {
            auto convGroup = new QGroupBox("Convolution Preset", m_optionsContainer);
            auto convForm = new QFormLayout(convGroup);
            convForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

            auto convCombo = new QComboBox(convGroup);
            convCombo->addItem("None", QString());
            for (const auto& preset : m_pipeline->convPresets) {
                convCombo->addItem(QString("%1  %2 · %3 taps")
                                       .arg(QString::fromStdString(preset.name))
                                       .arg(QString::fromStdString(preset.kindLabel()))
                                       .arg(preset.taps),
                                   preset.id.toString());
            }
            if (stage.convPresetId.has_value()) {
                int idx = convCombo->findData(stage.convPresetId->toString());
                if (idx >= 0)
                    convCombo->setCurrentIndex(idx);
            }
            connect(convCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, convCombo]() {
                QString idStr = convCombo->currentData().toString();
                if (idStr.isEmpty())
                    stage.convPresetId = std::nullopt;
                else
                    stage.convPresetId = QUuid::fromString(idStr);
                applyConfig();
                refreshUi();
            });
            addWidgetRow(convForm, "&Preset:", convCombo, convGroup);

            if (stage.convPresetId.has_value()) {
                auto it =
                    std::find_if(m_pipeline->convPresets.begin(), m_pipeline->convPresets.end(),
                                 [&stage](const ConvolutionPreset& p) { return p.id == stage.convPresetId.value(); });
                if (it != m_pipeline->convPresets.end()) {
                    int sampleRate = (m_dspController && m_dspController->devices())
                                         ? m_dspController->devices()->captureConfig.sampleRate
                                         : 48000;
                    auto availableRates = it->availableSampleRates();
                    int effectiveRate = sampleRate;
                    if (!availableRates.empty() &&
                        std::find(availableRates.begin(), availableRates.end(), sampleRate) == availableRates.end()) {
                        double targetLog = std::log(static_cast<double>(sampleRate));
                        effectiveRate =
                            *std::min_element(availableRates.begin(), availableRates.end(), [targetLog](int a, int b) {
                                return std::abs(std::log(static_cast<double>(a)) - targetLog) <
                                       std::abs(std::log(static_cast<double>(b)) - targetLog);
                            });
                    }
                    auto metaLbl = new QLabel(QString("Kind: %1  |  Taps: %2  |  Rate: %3 Hz  |  Latency: %4 ms")
                                                  .arg(QString::fromStdString(it->kindLabel()))
                                                  .arg(it->taps)
                                                  .arg(effectiveRate)
                                                  .arg(it->latencyMilliseconds(effectiveRate), 0, 'f', 1),
                                              convGroup);
                    convForm->addRow("Details:", metaLbl);

                    std::string irPath = it->irPath(effectiveRate);
                    if (!irPath.empty()) {
                        auto plot = new ConvolutionIRPlot(convGroup);
                        plot->setIRPath(irPath);
                        plot->setFixedHeight(110);
                        convForm->addRow("Impulse:", plot);
                    }
                }
            }
            containerLayout->addWidget(convGroup);
        }
    case StageType::Loudness: {
        auto loudGroup = new QGroupBox("Loudness Compensation", m_optionsContainer);
        auto loudForm = new QFormLayout(loudGroup);
        loudForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto refSlider = new QSlider(Qt::Horizontal, loudGroup);
        refSlider->setRange(-100, 20);
        refSlider->setSingleStep(1);
        refSlider->setPageStep(5);
        refSlider->setValue(static_cast<int>(stage.loudnessReference));
        auto refLbl = new QLabel(QString("%1 dB").arg(static_cast<int>(stage.loudnessReference)), loudGroup);
        connect(refSlider, &QSlider::valueChanged, [this, &stage, refLbl](int val) {
            stage.loudnessReference = val;
            refLbl->setText(QString("%1 dB").arg(val));
            applyConfig();
        });
        addSliderRow(loudForm, "&Reference Level:", refSlider, refLbl, loudGroup);

        auto lowSlider = new QSlider(Qt::Horizontal, loudGroup);
        lowSlider->setRange(0, 40);
        lowSlider->setSingleStep(1);
        lowSlider->setPageStep(5);
        lowSlider->setValue(static_cast<int>(stage.loudnessLowBoost * 2.0));
        auto lowLbl = new QLabel(QString("%1 dB").arg(stage.loudnessLowBoost, 0, 'f', 1), loudGroup);
        connect(lowSlider, &QSlider::valueChanged, [this, &stage, lowLbl](int val) {
            stage.loudnessLowBoost = val / 2.0;
            lowLbl->setText(QString("%1 dB").arg(stage.loudnessLowBoost, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(loudForm, "&Low Boost:", lowSlider, lowLbl, loudGroup);

        auto highSlider = new QSlider(Qt::Horizontal, loudGroup);
        highSlider->setRange(0, 40);
        highSlider->setSingleStep(1);
        highSlider->setPageStep(5);
        highSlider->setValue(static_cast<int>(stage.loudnessHighBoost * 2.0));
        auto highLbl = new QLabel(QString("%1 dB").arg(stage.loudnessHighBoost, 0, 'f', 1), loudGroup);
        connect(highSlider, &QSlider::valueChanged, [this, &stage, highLbl](int val) {
            stage.loudnessHighBoost = val / 2.0;
            highLbl->setText(QString("%1 dB").arg(stage.loudnessHighBoost, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(loudForm, "&High Boost:", highSlider, highLbl, loudGroup);

        auto faderCombo = new QComboBox(loudGroup);
        faderCombo->addItems({"Main", "Aux 1", "Aux 2", "Aux 3", "Aux 4"});
        faderCombo->setCurrentIndex(static_cast<int>(stage.loudnessFader));
        connect(faderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.loudnessFader = static_cast<Fader>(idx);
            applyConfig();
        });
        addWidgetRow(loudForm, "&Fader:", faderCombo, loudGroup);

        auto attChk = new QCheckBox("Attenuate Mid (instead of boosting extremes)", loudGroup);
        attChk->setChecked(stage.loudnessAttenuateMid);
        connect(attChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.loudnessAttenuateMid = chk;
            applyConfig();
        });
        loudForm->addRow("", attChk);

        containerLayout->addWidget(loudGroup);
        break;
    }

    case StageType::Emphasis: {
        auto empGroup = new QGroupBox("Emphasis", m_optionsContainer);
        auto empForm = new QFormLayout(empGroup);
        empForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto modeCombo = new QComboBox(empGroup);
        modeCombo->addItems({"De-Emphasis", "Pre-Emphasis"});
        modeCombo->setCurrentIndex(stage.emphasisMode == EmphasisMode::PreEmphasis ? 1 : 0);
        connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.emphasisMode = (idx == 1) ? EmphasisMode::PreEmphasis : EmphasisMode::DeEmphasis;
            applyConfig();
            refreshUi();
        });
        addWidgetRow(empForm, "&Mode:", modeCombo, empGroup);

        auto descLbl = new QLabel(QString::fromStdString(emphasisModeDescription(stage.emphasisMode)), empGroup);
        descLbl->setWordWrap(true);
        empForm->addRow("Description:", descLbl);

        containerLayout->addWidget(empGroup);
        break;
    }

    case StageType::DCProtection: {
        auto dcpGroup = new QGroupBox("DC Protection", m_optionsContainer);
        auto dcpBox = new QVBoxLayout(dcpGroup);
        auto dcpLbl = new QLabel(
            "First-order highpass at 7 Hz — removes DC offset and subsonic content on all selected channels.",
            dcpGroup);
        dcpLbl->setWordWrap(true);
        dcpBox->addWidget(dcpLbl);
        containerLayout->addWidget(dcpGroup);
        break;
    }

    case StageType::Gain: {
        auto gainGroup = new QGroupBox("Gain / Mute Settings", m_optionsContainer);
        auto gainForm = new QFormLayout(gainGroup);
        gainForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto gainSlider = new QSlider(Qt::Horizontal, gainGroup);
        gainSlider->setRange(-1500, 1500);
        gainSlider->setValue(static_cast<int>(stage.gainValue * 10.0));
        auto gainLbl = new QLabel(
            QString("%1%2 dB").arg(stage.gainValue >= 0 ? "+" : "").arg(stage.gainValue, 0, 'f', 1), gainGroup);

        auto resetBtn = new QPushButton("Reset", gainGroup);
        resetBtn->setFixedHeight(22);
        connect(resetBtn, &QPushButton::clicked, [this, &stage, gainSlider]() {
            stage.gainValue = 0.0;
            gainSlider->setValue(0);
            applyConfig();
        });

        connect(gainSlider, &QSlider::valueChanged, [this, &stage, gainLbl](int val) {
            stage.gainValue = val / 10.0;
            gainLbl->setText(QString("%1%2 dB").arg(stage.gainValue >= 0 ? "+" : "").arg(stage.gainValue, 0, 'f', 1));
            applyConfig();
        });

        addSliderRow(gainForm, "&Gain:", gainSlider, gainLbl, gainGroup, resetBtn);

        auto invChk = new QCheckBox("Invert Polarity", gainGroup);
        invChk->setChecked(stage.gainInverted);
        connect(invChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.gainInverted = chk;
            applyConfig();
        });
        gainForm->addRow("Polarity:", invChk);

        auto muteChk = new QCheckBox("Mute", gainGroup);
        muteChk->setChecked(stage.gainMuted);
        connect(muteChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.gainMuted = chk;
            applyConfig();
        });
        gainForm->addRow("Mute:", muteChk);

        containerLayout->addWidget(gainGroup);
        break;
    }

    case StageType::Delay: {
        auto delayGroup = new QGroupBox("Delay / Time Alignment", m_optionsContainer);
        auto delayForm = new QFormLayout(delayGroup);
        delayForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto unitCombo = new QComboBox(delayGroup);
        unitCombo->addItems({"Milliseconds (ms)", "Microseconds (μs)", "Seconds (s)", "Samples", "Millimeters (mm)"});
        unitCombo->setCurrentIndex(static_cast<int>(stage.delayUnit));
        connect(unitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.delayUnit = static_cast<DelayUnit>(idx);
            applyConfig();
            refreshUi();
        });
        addWidgetRow(delayForm, "&Unit:", unitCombo, delayGroup);

        double maxVal = 1000.0;
        switch (stage.delayUnit) {
        case DelayUnit::samples:
            maxVal = 96000.0;
            break;
        case DelayUnit::us:
            maxVal = 1000000.0;
            break;
        case DelayUnit::s:
            maxVal = 10.0;
            break;
        case DelayUnit::mm:
            maxVal = 1000.0;
            break;
        default:
            maxVal = 1000.0;
            break;
        }
        double stepVal = (stage.delayUnit == DelayUnit::samples) ? (stage.delaySubsample ? 0.01 : 1.0) : 0.1;

        auto delaySlider = new QSlider(Qt::Horizontal, delayGroup);
        int stepsCount = static_cast<int>(maxVal / stepVal);
        delaySlider->setRange(0, stepsCount);
        delaySlider->setValue(static_cast<int>(stage.delayValue / stepVal));

        static const char* unitSyms[] = {"ms", "μs", "s", "samples", "mm"};
        auto delayLbl = new QLabel(
            QString("%1 %2").arg(stage.delayValue, 0, 'f', 2).arg(unitSyms[static_cast<int>(stage.delayUnit)]),
            delayGroup);

        auto zeroBtn = new QPushButton("Zero", delayGroup);
        zeroBtn->setFixedHeight(22);
        connect(zeroBtn, &QPushButton::clicked, [this, &stage, delaySlider]() {
            stage.delayValue = 0.0;
            delaySlider->setValue(0);
            applyConfig();
        });

        connect(delaySlider, &QSlider::valueChanged, [this, &stage, delayLbl, stepVal](int val) {
            stage.delayValue = val * stepVal;
            static const char* uSyms[] = {"ms", "μs", "s", "samples", "mm"};
            delayLbl->setText(
                QString("%1 %2").arg(stage.delayValue, 0, 'f', 2).arg(uSyms[static_cast<int>(stage.delayUnit)]));
            applyConfig();
        });

        addSliderRow(delayForm, "&Delay:", delaySlider, delayLbl, delayGroup, zeroBtn);

        auto subChk = new QCheckBox("Subsample Delay (uses IIR allpass filter)", delayGroup);
        subChk->setChecked(stage.delaySubsample);
        connect(subChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.delaySubsample = chk;
            applyConfig();
            refreshUi();
        });
        delayForm->addRow("Subsample:", subChk);

        containerLayout->addWidget(delayGroup);
        break;
    }

    case StageType::Volume: {
        auto volGroup = new QGroupBox("Volume Control", m_optionsContainer);
        auto volForm = new QFormLayout(volGroup);
        volForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto faderCombo = new QComboBox(volGroup);
        faderCombo->addItems({"Aux 1", "Aux 2", "Aux 3", "Aux 4"});
        faderCombo->setCurrentIndex(std::max(0, static_cast<int>(stage.volumeFader) - 1));
        connect(faderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.volumeFader = static_cast<Fader>(idx + 1);
            applyConfig();
        });
        addWidgetRow(volForm, "&Fader:", faderCombo, volGroup);

        auto rampSlider = new QSlider(Qt::Horizontal, volGroup);
        rampSlider->setRange(0, 40);
        rampSlider->setValue(static_cast<int>(stage.volumeRampTime / 50.0));
        auto rampLbl = new QLabel(QString("%1 ms").arg(static_cast<int>(stage.volumeRampTime)), volGroup);
        connect(rampSlider, &QSlider::valueChanged, [this, &stage, rampLbl](int val) {
            stage.volumeRampTime = val * 50;
            rampLbl->setText(QString("%1 ms").arg(static_cast<int>(stage.volumeRampTime)));
            applyConfig();
        });
        addSliderRow(volForm, "&Ramp Time:", rampSlider, rampLbl, volGroup);

        auto limitSlider = new QSlider(Qt::Horizontal, volGroup);
        limitSlider->setRange(-500, 200);
        limitSlider->setValue(static_cast<int>(stage.volumeLimit * 10.0));
        auto limitLbl = new QLabel(
            QString("%1%2 dB").arg(stage.volumeLimit >= 0 ? "+" : "").arg(stage.volumeLimit, 0, 'f', 1), volGroup);
        connect(limitSlider, &QSlider::valueChanged, [this, &stage, limitLbl](int val) {
            stage.volumeLimit = val / 10.0;
            limitLbl->setText(
                QString("%1%2 dB").arg(stage.volumeLimit >= 0 ? "+" : "").arg(stage.volumeLimit, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(volForm, "&Limit:", limitSlider, limitLbl, volGroup);

        containerLayout->addWidget(volGroup);
        break;
    }

    case StageType::LookaheadLimiter: {
        auto limGroup = new QGroupBox("Lookahead Peak Limiter", m_optionsContainer);
        auto limForm = new QFormLayout(limGroup);
        limForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto limitSlider = new QSlider(Qt::Horizontal, limGroup);
        limitSlider->setRange(-300, 0);
        limitSlider->setValue(static_cast<int>(stage.lookaheadLimit * 10.0));
        auto limitLbl = new QLabel(QString("%1 dB").arg(stage.lookaheadLimit, 0, 'f', 1), limGroup);
        connect(limitSlider, &QSlider::valueChanged, [this, &stage, limitLbl](int val) {
            stage.lookaheadLimit = val / 10.0;
            limitLbl->setText(QString("%1 dB").arg(stage.lookaheadLimit, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(limForm, "&Limit:", limitSlider, limitLbl, limGroup);

        auto attSlider = new QSlider(Qt::Horizontal, limGroup);
        attSlider->setRange(1, 10000);
        attSlider->setValue(static_cast<int>(stage.lookaheadAttack * 10.0));
        auto attLbl = new QLabel(QString("%1 %2")
                                     .arg(stage.lookaheadAttack, 0, 'f', 1)
                                     .arg(QString::fromStdString(timeUnitToString(stage.lookaheadAttackUnit))),
                                 limGroup);
        connect(attSlider, &QSlider::valueChanged, [this, &stage, attLbl](int val) {
            stage.lookaheadAttack = val / 10.0;
            attLbl->setText(QString("%1 %2")
                                .arg(stage.lookaheadAttack, 0, 'f', 1)
                                .arg(QString::fromStdString(timeUnitToString(stage.lookaheadAttackUnit))));
            applyConfig();
        });
        addSliderRow(limForm, "&Attack:", attSlider, attLbl, limGroup);

        auto relSlider = new QSlider(Qt::Horizontal, limGroup);
        relSlider->setRange(5, 1000);
        relSlider->setSingleStep(5);
        relSlider->setPageStep(5);
        relSlider->setValue(static_cast<int>(stage.lookaheadRelease));
        auto relLbl = new QLabel(QString("%1 %2")
                                     .arg(static_cast<int>(stage.lookaheadRelease))
                                     .arg(QString::fromStdString(timeUnitToString(stage.lookaheadReleaseUnit))),
                                 limGroup);
        connect(relSlider, &QSlider::valueChanged, [this, &stage, relLbl](int val) {
            stage.lookaheadRelease = val;
            relLbl->setText(
                QString("%1 %2").arg(val).arg(QString::fromStdString(timeUnitToString(stage.lookaheadReleaseUnit))));
            applyConfig();
        });
        addSliderRow(limForm, "&Release:", relSlider, relLbl, limGroup);

        containerLayout->addWidget(limGroup);
        break;
    }

    case StageType::LookaheadLimiterProc: {
        auto limGroup = new QGroupBox("Lookahead Peak Limiter Processor", m_optionsContainer);
        auto limForm = new QFormLayout(limGroup);
        limForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto limitSlider = new QSlider(Qt::Horizontal, limGroup);
        limitSlider->setRange(-300, 0);
        limitSlider->setValue(static_cast<int>(stage.lookaheadLimit * 10.0));
        auto limitLbl = new QLabel(QString("%1 dB").arg(stage.lookaheadLimit, 0, 'f', 1), limGroup);
        connect(limitSlider, &QSlider::valueChanged, [this, &stage, limitLbl](int val) {
            stage.lookaheadLimit = val / 10.0;
            limitLbl->setText(QString("%1 dB").arg(stage.lookaheadLimit, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(limForm, "&Limit:", limitSlider, limitLbl, limGroup);

        auto attSlider = new QSlider(Qt::Horizontal, limGroup);
        attSlider->setRange(1, 10000);
        attSlider->setValue(static_cast<int>(stage.lookaheadAttack * 10.0));
        auto attLbl = new QLabel(QString("%1 %2")
                                     .arg(stage.lookaheadAttack, 0, 'f', 1)
                                     .arg(QString::fromStdString(timeUnitToString(stage.lookaheadAttackUnit))),
                                 limGroup);
        connect(attSlider, &QSlider::valueChanged, [this, &stage, attLbl](int val) {
            stage.lookaheadAttack = val / 10.0;
            attLbl->setText(QString("%1 %2")
                                .arg(stage.lookaheadAttack, 0, 'f', 1)
                                .arg(QString::fromStdString(timeUnitToString(stage.lookaheadAttackUnit))));
            applyConfig();
        });
        addSliderRow(limForm, "&Attack:", attSlider, attLbl, limGroup);

        auto relSlider = new QSlider(Qt::Horizontal, limGroup);
        relSlider->setRange(5, 1000);
        relSlider->setSingleStep(5);
        relSlider->setPageStep(5);
        relSlider->setValue(static_cast<int>(stage.lookaheadRelease));
        auto relLbl = new QLabel(QString("%1 %2")
                                     .arg(static_cast<int>(stage.lookaheadRelease))
                                     .arg(QString::fromStdString(timeUnitToString(stage.lookaheadReleaseUnit))),
                                 limGroup);
        connect(relSlider, &QSlider::valueChanged, [this, &stage, relLbl](int val) {
            stage.lookaheadRelease = val;
            relLbl->setText(
                QString("%1 %2").arg(val).arg(QString::fromStdString(timeUnitToString(stage.lookaheadReleaseUnit))));
            applyConfig();
        });
        addSliderRow(limForm, "&Release:", relSlider, relLbl, limGroup);

        auto delayProcOnlyCheck = new QCheckBox("Delay Processed Channels Only", limGroup);
        delayProcOnlyCheck->setChecked(stage.lookaheadDelayProcessedOnly);
        connect(delayProcOnlyCheck, &QCheckBox::toggled, [this, &stage](bool checked) {
            stage.lookaheadDelayProcessedOnly = checked;
            applyConfig();
        });
        limForm->addRow("&Options:", delayProcOnlyCheck);

        auto monitorScroll = createChannelSelectorWidget(
            incomingChannels,
            [this]() -> std::vector<int> {
                auto st = currentStage();
                return st ? st->monitorChannels : std::vector<int>{};
            },
            [this](const std::vector<int>& chs) {
                auto st = currentStage();
                if (st) {
                    st->monitorChannels = chs;
                    applyConfig();
                }
            },
            limGroup);
        limForm->addRow("Monitor Channels:", monitorScroll);

        containerLayout->addWidget(limGroup);
        break;
    }

    case StageType::Clipper: {
        auto limGroup = new QGroupBox("Clipper", m_optionsContainer);
        auto limForm = new QFormLayout(limGroup);
        limForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto limitSlider = new QSlider(Qt::Horizontal, limGroup);
        limitSlider->setRange(-300, 0);
        limitSlider->setValue(static_cast<int>(stage.clipperLimit * 10.0));
        auto limitLbl = new QLabel(QString("%1 dB").arg(stage.clipperLimit, 0, 'f', 1), limGroup);
        connect(limitSlider, &QSlider::valueChanged, [this, &stage, limitLbl](int val) {
            stage.clipperLimit = val / 10.0;
            limitLbl->setText(QString("%1 dB").arg(stage.clipperLimit, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(limForm, "&Limit:", limitSlider, limitLbl, limGroup);

        auto softChk = new QCheckBox("Enable Soft Clipping", limGroup);
        softChk->setChecked(stage.clipperSoftClip);
        connect(softChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.clipperSoftClip = chk;
            applyConfig();
        });
        limForm->addRow("Soft Clip:", softChk);

        containerLayout->addWidget(limGroup);
        break;
    }

    case StageType::MatrixMixer: {
        stage.mixerChannelsIn = incomingChannels;

        auto cfgGroup = new QGroupBox("Channel Configuration", m_optionsContainer);
        auto cfgForm = new QFormLayout(cfgGroup);
        cfgForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto inLabel = new QLabel(QString("%1 Channels (Auto)").arg(incomingChannels), cfgGroup);
        cfgForm->addRow("Input Channels:", inLabel);

        auto outCombo = new QComboBox(cfgGroup);
        for (int c = 1; c <= 16; ++c)
            outCombo->addItem(QString("%1 Channels").arg(c), c);
        outCombo->setCurrentIndex(stage.mixerChannelsOut - 1);
        connect(outCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, outCombo](int idx) {
            stage.mixerChannelsOut = outCombo->itemData(idx).toInt();
            applyConfig();
            refreshUi();
        });
        addWidgetRow(cfgForm, "&Output Channels:", outCombo, cfgGroup);

        containerLayout->addWidget(cfgGroup);

        auto matrixGroup = new QGroupBox("Matrix Mixer Mapping", m_optionsContainer);
        auto matrixVBox = new QVBoxLayout(matrixGroup);
        matrixVBox->setSpacing(12);

        int rows = stage.mixerChannelsOut;
        int cols = incomingChannels;

        auto table = new QTableWidget(rows, cols, matrixGroup);
        table->horizontalHeader()->setDefaultSectionSize(120);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table->verticalHeader()->setDefaultSectionSize(95);
        table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table->setMinimumHeight(std::min(520, rows * 95 + 40));

        QStringList headers;
        for (int c = 0; c < cols; ++c)
            headers << QString("Ch %1").arg(c + 1);
        table->setHorizontalHeaderLabels(headers);

        QStringList rowLabels;
        for (int r = 0; r < rows; ++r)
            rowLabels << QString("Ch %1").arg(r + 1);
        table->setVerticalHeaderLabels(rowLabels);

        auto cornerBtn = table->findChild<QAbstractButton*>();
        if (cornerBtn) {
            cornerBtn->setText("Out \\ In");
        }

        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                table->setCellWidget(r, c, createMatrixCellWidget(stage, r, c, table));
            }
        }
        matrixVBox->addWidget(table);

        auto resetBtn = new QPushButton("Reset to 1:1 Passthrough", matrixGroup);
        resetBtn->setFixedHeight(24);
        connect(resetBtn, &QPushButton::clicked, [this, &stage, rows, cols]() {
            int minCh = std::min(rows, cols);
            stage.mixerMappings.clear();
            for (int r = 0; r < rows; ++r) {
                int src = r < minCh ? r : 0;
                stage.mixerMappings.push_back(
                    MixerMapping{r, {MixerSource{src, 0.0, std::nullopt, std::nullopt, std::nullopt}}, std::nullopt});
            }
            applyConfig();
            refreshUi();
        });
        matrixVBox->addWidget(resetBtn, 0, Qt::AlignLeft);

        containerLayout->addWidget(matrixGroup);
        break;
    }

    case StageType::Compressor: {
        auto compGroup = new QGroupBox("Dynamics Compressor", m_optionsContainer);
        auto compForm = new QFormLayout(compGroup);
        compForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto thSlider = new QSlider(Qt::Horizontal, compGroup);
        thSlider->setRange(-600, 0);
        thSlider->setSingleStep(5);
        thSlider->setPageStep(50);
        thSlider->setValue(static_cast<int>(stage.compressorThreshold * 10.0));
        auto thLbl = new QLabel(QString("%1 dB").arg(stage.compressorThreshold, 0, 'f', 1), compGroup);
        connect(thSlider, &QSlider::valueChanged, [this, &stage, thLbl](int val) {
            stage.compressorThreshold = val / 10.0;
            thLbl->setText(QString("%1 dB").arg(stage.compressorThreshold, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(compForm, "&Threshold:", thSlider, thLbl, compGroup);

        auto ratioSlider = new QSlider(Qt::Horizontal, compGroup);
        ratioSlider->setRange(10, 200);
        ratioSlider->setSingleStep(1);
        ratioSlider->setPageStep(10);
        ratioSlider->setValue(static_cast<int>(stage.compressorRatio * 10.0));
        auto ratioLbl = new QLabel(QString("%1:1").arg(stage.compressorRatio, 0, 'f', 1), compGroup);
        connect(ratioSlider, &QSlider::valueChanged, [this, &stage, ratioLbl](int val) {
            stage.compressorRatio = val / 10.0;
            ratioLbl->setText(QString("%1:1").arg(stage.compressorRatio, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(compForm, "&Ratio:", ratioSlider, ratioLbl, compGroup);

        auto attSlider = new QSlider(Qt::Horizontal, compGroup);
        attSlider->setRange(1, 1000);
        attSlider->setSingleStep(1);
        attSlider->setPageStep(10);
        attSlider->setValue(static_cast<int>(stage.compressorAttack * 10.0));
        auto attLbl = new QLabel(QString("%1 %2")
                                     .arg(stage.compressorAttack, 0, 'f', 1)
                                     .arg(QString::fromStdString(timeUnitToString(stage.compressorAttackUnit))),
                                 compGroup);
        connect(attSlider, &QSlider::valueChanged, [this, &stage, attLbl](int val) {
            stage.compressorAttack = val / 10.0;
            attLbl->setText(QString("%1 %2")
                                .arg(stage.compressorAttack, 0, 'f', 1)
                                .arg(QString::fromStdString(timeUnitToString(stage.compressorAttackUnit))));
            applyConfig();
        });
        addSliderRow(compForm, "&Attack:", attSlider, attLbl, compGroup);

        auto relSlider = new QSlider(Qt::Horizontal, compGroup);
        relSlider->setRange(5, 1000);
        relSlider->setSingleStep(5);
        relSlider->setPageStep(50);
        relSlider->setValue(static_cast<int>(stage.compressorRelease));
        auto relLbl = new QLabel(QString("%1 %2")
                                     .arg(static_cast<int>(stage.compressorRelease))
                                     .arg(QString::fromStdString(timeUnitToString(stage.compressorReleaseUnit))),
                                 compGroup);
        connect(relSlider, &QSlider::valueChanged, [this, &stage, relLbl](int val) {
            stage.compressorRelease = val;
            relLbl->setText(
                QString("%1 %2").arg(val).arg(QString::fromStdString(timeUnitToString(stage.compressorReleaseUnit))));
            applyConfig();
        });
        addSliderRow(compForm, "&Release:", relSlider, relLbl, compGroup);

        auto mkSlider = new QSlider(Qt::Horizontal, compGroup);
        mkSlider->setRange(0, 300);
        mkSlider->setSingleStep(5);
        mkSlider->setPageStep(50);
        mkSlider->setValue(static_cast<int>(stage.compressorMakeupGain * 10.0));
        auto mkLbl = new QLabel(QString("%1%2 dB")
                                    .arg(stage.compressorMakeupGain >= 0 ? "+" : "")
                                    .arg(stage.compressorMakeupGain, 0, 'f', 1),
                                compGroup);
        connect(mkSlider, &QSlider::valueChanged, [this, &stage, mkLbl](int val) {
            stage.compressorMakeupGain = val / 10.0;
            mkLbl->setText(QString("%1%2 dB")
                               .arg(stage.compressorMakeupGain >= 0 ? "+" : "")
                               .arg(stage.compressorMakeupGain, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(compForm, "&Makeup Gain:", mkSlider, mkLbl, compGroup);

        auto monitorScroll = createChannelSelectorWidget(
            incomingChannels,
            [this]() -> std::vector<int> {
                auto st = currentStage();
                return st ? st->monitorChannels : std::vector<int>{};
            },
            [this](const std::vector<int>& chs) {
                auto st = currentStage();
                if (st) {
                    st->monitorChannels = chs;
                    applyConfig();
                }
            },
            compGroup);
        compForm->addRow("Monitor Channels:", monitorScroll);

        auto softChk = new QCheckBox("Enable Soft Clip", compGroup);
        softChk->setChecked(stage.compressorSoftClip);
        compForm->addRow("Soft Clip:", softChk);

        auto clipSlider = new QSlider(Qt::Horizontal, compGroup);
        clipSlider->setRange(-100, 0);
        clipSlider->setValue(static_cast<int>(stage.compressorClipLimit * 10.0));
        auto clipLbl = new QLabel(QString("%1 dB").arg(stage.compressorClipLimit, 0, 'f', 1), compGroup);
        connect(clipSlider, &QSlider::valueChanged, [this, &stage, clipLbl](int val) {
            stage.compressorClipLimit = val / 10.0;
            clipLbl->setText(QString("%1 dB").arg(stage.compressorClipLimit, 0, 'f', 1));
            applyConfig();
        });

        auto clipField = createSliderField(clipSlider, clipLbl);
        auto clipFieldLabel = new QLabel("Clip &Limit:", compGroup);
        clipFieldLabel->setBuddy(clipSlider);
        compForm->addRow(clipFieldLabel, clipField);

        compForm->setRowVisible(clipField, stage.compressorSoftClip);
        connect(softChk, &QCheckBox::toggled, [this, &stage, compForm, clipField](bool chk) {
            stage.compressorSoftClip = chk;
            compForm->setRowVisible(clipField, chk);
            applyConfig();
        });

        containerLayout->addWidget(compGroup);
        break;
    }

    case StageType::NoiseGate: {
        auto gateGroup = new QGroupBox("Noise Gate", m_optionsContainer);
        auto gateForm = new QFormLayout(gateGroup);
        gateForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto thSlider = new QSlider(Qt::Horizontal, gateGroup);
        thSlider->setRange(-1000, 0);
        thSlider->setSingleStep(5);
        thSlider->setPageStep(50);
        thSlider->setValue(static_cast<int>(stage.gateThreshold * 10.0));
        auto thLbl = new QLabel(QString("%1 dB").arg(stage.gateThreshold, 0, 'f', 1), gateGroup);
        connect(thSlider, &QSlider::valueChanged, [this, &stage, thLbl](int val) {
            stage.gateThreshold = val / 10.0;
            thLbl->setText(QString("%1 dB").arg(stage.gateThreshold, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(gateForm, "&Threshold:", thSlider, thLbl, gateGroup);

        auto attenSlider = new QSlider(Qt::Horizontal, gateGroup);
        attenSlider->setRange(-1000, 0);
        attenSlider->setSingleStep(5);
        attenSlider->setPageStep(50);
        attenSlider->setValue(static_cast<int>(stage.gateAttenuation * 10.0));
        auto attenLbl = new QLabel(QString("%1 dB").arg(stage.gateAttenuation, 0, 'f', 1), gateGroup);
        connect(attenSlider, &QSlider::valueChanged, [this, &stage, attenLbl](int val) {
            stage.gateAttenuation = val / 10.0;
            attenLbl->setText(QString("%1 dB").arg(stage.gateAttenuation, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(gateForm, "Atten&uation:", attenSlider, attenLbl, gateGroup);

        auto attSlider = new QSlider(Qt::Horizontal, gateGroup);
        attSlider->setRange(1, 1000);
        attSlider->setSingleStep(1);
        attSlider->setPageStep(10);
        attSlider->setValue(static_cast<int>(stage.gateAttack * 10.0));
        auto attLbl = new QLabel(QString("%1 %2")
                                     .arg(stage.gateAttack, 0, 'f', 1)
                                     .arg(QString::fromStdString(timeUnitToString(stage.gateAttackUnit))),
                                 gateGroup);
        connect(attSlider, &QSlider::valueChanged, [this, &stage, attLbl](int val) {
            stage.gateAttack = val / 10.0;
            attLbl->setText(QString("%1 %2")
                                .arg(stage.gateAttack, 0, 'f', 1)
                                .arg(QString::fromStdString(timeUnitToString(stage.gateAttackUnit))));
            applyConfig();
        });
        addSliderRow(gateForm, "&Attack:", attSlider, attLbl, gateGroup);

        auto relSlider = new QSlider(Qt::Horizontal, gateGroup);
        relSlider->setRange(5, 1000);
        relSlider->setSingleStep(5);
        relSlider->setPageStep(50);
        relSlider->setValue(static_cast<int>(stage.gateRelease));
        auto relLbl = new QLabel(QString("%1 %2")
                                     .arg(static_cast<int>(stage.gateRelease))
                                     .arg(QString::fromStdString(timeUnitToString(stage.gateReleaseUnit))),
                                 gateGroup);
        connect(relSlider, &QSlider::valueChanged, [this, &stage, relLbl](int val) {
            stage.gateRelease = val;
            relLbl->setText(
                QString("%1 %2").arg(val).arg(QString::fromStdString(timeUnitToString(stage.gateReleaseUnit))));
            applyConfig();
        });
        addSliderRow(gateForm, "&Release:", relSlider, relLbl, gateGroup);

        auto monitorScroll = createChannelSelectorWidget(
            incomingChannels,
            [this]() -> std::vector<int> {
                auto st = currentStage();
                return st ? st->monitorChannels : std::vector<int>{};
            },
            [this](const std::vector<int>& chs) {
                auto st = currentStage();
                if (st) {
                    st->monitorChannels = chs;
                    applyConfig();
                }
            },
            gateGroup);
        gateForm->addRow("Monitor Channels:", monitorScroll);

        containerLayout->addWidget(gateGroup);
        break;
    }

    case StageType::RACE: {
        auto raceGroup = new QGroupBox("RACE Crosstalk Cancellation", m_optionsContainer);
        auto raceForm = new QFormLayout(raceGroup);
        raceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto descLbl = new QLabel("Receiver Active Crosstalk Cancellation (RACE) implements a 3D audio effect for "
                                  "speaker playback by canceling acoustic crosstalk between two channels.",
                                  raceGroup);
        descLbl->setWordWrap(true);
        raceForm->addRow(descLbl);

        auto unitCombo = new QComboBox(raceGroup);
        unitCombo->addItems({"Milliseconds (ms)", "Microseconds (μs)", "Seconds (s)", "Samples", "Millimeters (mm)"});
        unitCombo->setCurrentIndex(static_cast<int>(stage.raceDelayUnit));
        connect(unitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
            auto st = currentStage();
            if (!st)
                return;
            st->raceDelayUnit = static_cast<DelayUnit>(idx);
            applyConfig();
            refreshUi();
        });
        addWidgetRow(raceForm, "&Delay Unit:", unitCombo, raceGroup);

        double minVal = 0.01;
        double maxVal = 2.0;
        double stepVal = 0.01;
        switch (stage.raceDelayUnit) {
        case DelayUnit::samples:
            minVal = 1.0;
            maxVal = 100.0;
            stepVal = stage.raceSubsampleDelay ? 0.01 : 1.0;
            break;
        case DelayUnit::us:
            minVal = 5.0;
            maxVal = 2000.0;
            stepVal = 1.0;
            break;
        case DelayUnit::s:
            minVal = 0.0001;
            maxVal = 1.0;
            stepVal = 0.001;
            break;
        case DelayUnit::mm:
            minVal = 2.0;
            maxVal = 700.0;
            stepVal = 1.0;
            break;
        case DelayUnit::ms:
            minVal = 0.01;
            maxVal = 2.0;
            stepVal = 0.01;
            break;
        }

        int stepsCount = static_cast<int>(std::round((maxVal - minVal) / stepVal));
        auto delaySlider = new QSlider(Qt::Horizontal, raceGroup);
        delaySlider->setRange(0, stepsCount);
        delaySlider->setValue(static_cast<int>(std::round((stage.raceDelay - minVal) / stepVal)));

        static const char* unitSymbols[] = {"ms", "μs", "s", "samples", "mm"};
        auto delayLbl = new QLabel(
            QString("%1 %2").arg(stage.raceDelay, 0, 'f', 2).arg(unitSymbols[static_cast<int>(stage.raceDelayUnit)]),
            raceGroup);
        connect(delaySlider, &QSlider::valueChanged, [this, delayLbl, minVal, stepVal](int val) {
            auto st = currentStage();
            if (!st)
                return;
            st->raceDelay = minVal + val * stepVal;
            static const char* uSyms[] = {"ms", "μs", "s", "samples", "mm"};
            delayLbl->setText(
                QString("%1 %2").arg(st->raceDelay, 0, 'f', 2).arg(uSyms[static_cast<int>(st->raceDelayUnit)]));
            applyConfig();
        });
        addSliderRow(raceForm, "&Delay:", delaySlider, delayLbl, raceGroup);

        auto attenSlider = new QSlider(Qt::Horizontal, raceGroup);
        attenSlider->setRange(10, 200);
        attenSlider->setValue(static_cast<int>(stage.raceAttenuation * 10.0));
        auto attenLbl = new QLabel(QString("%1 dB").arg(stage.raceAttenuation, 0, 'f', 1), raceGroup);
        connect(attenSlider, &QSlider::valueChanged, [this, attenLbl](int val) {
            auto st = currentStage();
            if (!st)
                return;
            st->raceAttenuation = val / 10.0;
            attenLbl->setText(QString("%1 dB").arg(st->raceAttenuation, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(raceForm, "Atten&uation:", attenSlider, attenLbl, raceGroup);

        auto subChk = new QCheckBox("Subsample Delay (uses IIR allpass filter)", raceGroup);
        subChk->setChecked(stage.raceSubsampleDelay);
        connect(subChk, &QCheckBox::toggled, [this](bool chk) {
            auto st = currentStage();
            if (!st)
                return;
            st->raceSubsampleDelay = chk;
            applyConfig();
            refreshUi();
        });
        raceForm->addRow("Subsample:", subChk);

        containerLayout->addWidget(raceGroup);
        break;
    }

    case StageType::Dither: {
        auto ditherGroup = new QGroupBox("Dither Noise Shaping", m_optionsContainer);
        auto ditherForm = new QFormLayout(ditherGroup);
        ditherForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto typeCombo = new QComboBox(ditherGroup);
        static const std::vector<DitherType> types = {DitherType::None,
                                                      DitherType::Flat,
                                                      DitherType::Highpass,
                                                      DitherType::Fweighted441,
                                                      DitherType::FweightedLong441,
                                                      DitherType::FweightedShort441,
                                                      DitherType::Gesemann441,
                                                      DitherType::Gesemann48,
                                                      DitherType::Lipshitz441,
                                                      DitherType::LipshitzLong441,
                                                      DitherType::Shibata441,
                                                      DitherType::ShibataHigh441,
                                                      DitherType::ShibataLow441,
                                                      DitherType::Shibata48,
                                                      DitherType::ShibataHigh48,
                                                      DitherType::ShibataLow48,
                                                      DitherType::Shibata882,
                                                      DitherType::ShibataLow882,
                                                      DitherType::Shibata96,
                                                      DitherType::ShibataLow96,
                                                      DitherType::Shibata192,
                                                      DitherType::ShibataLow192};
        auto ditherLabel = [](DitherType t) -> QString {
            switch (t) {
            case DitherType::None:
                return "None";
            case DitherType::Flat:
                return "Flat";
            case DitherType::Highpass:
                return "Highpass";
            case DitherType::Fweighted441:
                return "F-weighted 44.1k";
            case DitherType::FweightedLong441:
                return "F-weighted Long 44.1k";
            case DitherType::FweightedShort441:
                return "F-weighted Short 44.1k";
            case DitherType::Gesemann441:
                return "Gesemann 44.1k";
            case DitherType::Gesemann48:
                return "Gesemann 48k";
            case DitherType::Lipshitz441:
                return "Lipshitz 44.1k";
            case DitherType::LipshitzLong441:
                return "Lipshitz Long 44.1k";
            case DitherType::Shibata441:
                return "Shibata 44.1k";
            case DitherType::ShibataHigh441:
                return "Shibata High 44.1k";
            case DitherType::ShibataLow441:
                return "Shibata Low 44.1k";
            case DitherType::Shibata48:
                return "Shibata 48k";
            case DitherType::ShibataHigh48:
                return "Shibata High 48k";
            case DitherType::ShibataLow48:
                return "Shibata Low 48k";
            case DitherType::Shibata96:
                return "Shibata 96k";
            case DitherType::ShibataLow96:
                return "Shibata Low 96k";
            default:
                return QString::fromStdString(ditherTypeToString(t));
            }
        };
        for (auto t : types) {
            typeCombo->addItem(ditherLabel(t), static_cast<int>(t));
        }
        int curIdx = 0;
        for (size_t i = 0; i < types.size(); ++i) {
            if (types[i] == stage.ditherType) {
                curIdx = static_cast<int>(i);
                break;
            }
        }
        typeCombo->setCurrentIndex(curIdx);
        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            if (idx >= 0 && idx < static_cast<int>(types.size())) {
                stage.ditherType = types[idx];
                applyConfig();
            }
        });
        addWidgetRow(ditherForm, "&Type:", typeCombo, ditherGroup);

        auto bitsCombo = new QComboBox(ditherGroup);
        bitsCombo->addItems({"16-bit", "24-bit", "32-bit", "8-bit (Lofi)"});
        int bIdx = 0;
        if (stage.ditherBits == 24)
            bIdx = 1;
        else if (stage.ditherBits == 32)
            bIdx = 2;
        else if (stage.ditherBits == 8)
            bIdx = 3;
        bitsCombo->setCurrentIndex(bIdx);
        connect(bitsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            static const int bVals[] = {16, 24, 32, 8};
            stage.ditherBits = bVals[idx];
            applyConfig();
        });
        addWidgetRow(ditherForm, "&Bit Depth:", bitsCombo, ditherGroup);

        auto ampSlider = new QSlider(Qt::Horizontal, ditherGroup);
        ampSlider->setRange(0, 1000);
        ampSlider->setSingleStep(1);
        ampSlider->setPageStep(10);
        ampSlider->setValue(static_cast<int>(stage.ditherAmplitude * 10.0));
        auto ampLbl = new QLabel(QString("%1").arg(stage.ditherAmplitude, 0, 'f', 1), ditherGroup);
        connect(ampSlider, &QSlider::valueChanged, [this, &stage, ampLbl](int val) {
            stage.ditherAmplitude = val / 10.0;
            ampLbl->setText(QString("%1").arg(stage.ditherAmplitude, 0, 'f', 1));
            applyConfig();
        });
        addSliderRow(ditherForm, "&Amplitude:", ampSlider, ampLbl, ditherGroup);

        containerLayout->addWidget(ditherGroup);
        break;
    }

    case StageType::DiffEq: {
        auto deGroup = new QGroupBox("Differential Equation Filter", m_optionsContainer);
        auto deForm = new QFormLayout(deGroup);
        deForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto descLbl = new QLabel(
            "Direct form II IIR filter coefficients. Specify as comma-separated lists of decimal numbers.", deGroup);
        descLbl->setWordWrap(true);
        deForm->addRow(descLbl);

        auto bEdit = new QLineEdit(QString::fromStdString(stage.diffEqB), deGroup);
        bEdit->setPlaceholderText("e.g. 1.0, 0.5, 0.25");
        connect(bEdit, &QLineEdit::editingFinished, [this, bEdit]() {
            auto st = currentStage();
            if (!st)
                return;
            st->diffEqB = bEdit->text().toStdString();
            applyConfig();
        });
        addWidgetRow(deForm, "&Feedforward (b):", bEdit, deGroup);

        auto aEdit = new QLineEdit(QString::fromStdString(stage.diffEqA), deGroup);
        aEdit->setPlaceholderText("e.g. 1.0, -0.5, 0.1");
        connect(aEdit, &QLineEdit::editingFinished, [this, aEdit]() {
            auto st = currentStage();
            if (!st)
                return;
            st->diffEqA = aEdit->text().toStdString();
            applyConfig();
        });
        addWidgetRow(deForm, "Feed&back (a):", aEdit, deGroup);

        containerLayout->addWidget(deGroup);
        break;
    }

    case StageType::BiquadCombo: {
        auto comboGroup = new QGroupBox("Biquad Combo / Crossovers", m_optionsContainer);
        auto comboForm = new QFormLayout(comboGroup);
        comboForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto typeCombo = new QComboBox(comboGroup);
        typeCombo->addItem("Butterworth Lowpass", static_cast<int>(BiquadComboType::ButterworthLowpass));
        typeCombo->addItem("Butterworth Highpass", static_cast<int>(BiquadComboType::ButterworthHighpass));
        typeCombo->addItem("Linkwitz-Riley Lowpass", static_cast<int>(BiquadComboType::LinkwitzRileyLowpass));
        typeCombo->addItem("Linkwitz-Riley Highpass", static_cast<int>(BiquadComboType::LinkwitzRileyHighpass));
        typeCombo->addItem("Tilt", static_cast<int>(BiquadComboType::Tilt));
        typeCombo->addItem("Five-Point PEQ", static_cast<int>(BiquadComboType::FivePointPeq));

        int curTypeIdx = typeCombo->findData(static_cast<int>(stage.comboType));
        typeCombo->setCurrentIndex(curTypeIdx >= 0 ? curTypeIdx : 0);
        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, typeCombo](int idx) {
            auto st = currentStage();
            if (!st)
                return;
            st->comboType = static_cast<BiquadComboType>(typeCombo->itemData(idx).toInt());
            applyConfig();
            refreshUi();
        });
        addWidgetRow(comboForm, "Combo &Type:", typeCombo, comboGroup);

        if (stage.comboType != BiquadComboType::FivePointPeq) {
            auto freqSlider = new QSlider(Qt::Horizontal, comboGroup);
            freqSlider->setRange(20, 20000);
            freqSlider->setValue(static_cast<int>(stage.comboFreq));

            auto freqLbl = new QLabel(QString("%1 Hz").arg(static_cast<int>(stage.comboFreq)), comboGroup);
            connect(freqSlider, &QSlider::valueChanged, [this, freqLbl](int val) {
                auto st = currentStage();
                if (!st)
                    return;
                st->comboFreq = val;
                freqLbl->setText(QString("%1 Hz").arg(val));
                applyConfig();
            });

            addSliderRow(comboForm, "&Frequency:", freqSlider, freqLbl, comboGroup);
        }

        if (stage.comboType == BiquadComboType::ButterworthLowpass ||
            stage.comboType == BiquadComboType::ButterworthHighpass ||
            stage.comboType == BiquadComboType::LinkwitzRileyLowpass ||
            stage.comboType == BiquadComboType::LinkwitzRileyHighpass) {
            auto orderCombo = new QComboBox(comboGroup);
            orderCombo->addItem("2nd Order (12 dB/oct)", 2);
            orderCombo->addItem("4th Order (24 dB/oct)", 4);
            orderCombo->addItem("6th Order (36 dB/oct)", 6);
            orderCombo->addItem("8th Order (48 dB/oct)", 8);
            int idx = orderCombo->findData(stage.comboOrder);
            orderCombo->setCurrentIndex(idx >= 0 ? idx : 0);
            connect(orderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, orderCombo](int index) {
                auto st = currentStage();
                if (!st)
                    return;
                st->comboOrder = orderCombo->itemData(index).toInt();
                applyConfig();
            });
            addWidgetRow(comboForm, "Filter &Order:", orderCombo, comboGroup);
        }

        if (stage.comboType == BiquadComboType::Tilt) {
            auto gainSlider = new QSlider(Qt::Horizontal, comboGroup);
            gainSlider->setRange(-999, 999);
            gainSlider->setValue(static_cast<int>(stage.comboGain * 10.0));
            auto gainLbl = new QLabel(
                QString("%1%2 dB").arg(stage.comboGain >= 0 ? "+" : "").arg(stage.comboGain, 0, 'f', 1), comboGroup);
            connect(gainSlider, &QSlider::valueChanged, [this, gainLbl](int val) {
                auto st = currentStage();
                if (!st)
                    return;
                st->comboGain = val / 10.0;
                gainLbl->setText(QString("%1%2 dB").arg(st->comboGain >= 0 ? "+" : "").arg(st->comboGain, 0, 'f', 1));
                applyConfig();
            });
            addSliderRow(comboForm, "&Gain:", gainSlider, gainLbl, comboGroup);
        }

        containerLayout->addWidget(comboGroup);

        if (stage.comboType == BiquadComboType::FivePointPeq) {
            auto peqGroup = new QGroupBox("5-Point Parametric EQ", m_optionsContainer);
            auto peqGrid = new QGridLayout(peqGroup);
            peqGrid->setHorizontalSpacing(16);
            peqGrid->setVerticalSpacing(8);

            auto hdr1 = new QLabel("Band", peqGroup);
            QFont hdrFont = hdr1->font();
            hdrFont.setBold(true);
            hdr1->setFont(hdrFont);

            auto hdr2 = new QLabel("Frequency (Hz)", peqGroup);
            hdr2->setFont(hdrFont);
            auto hdr3 = new QLabel("Gain (dB)", peqGroup);
            hdr3->setFont(hdrFont);
            auto hdr4 = new QLabel("Q Factor", peqGroup);
            hdr4->setFont(hdrFont);

            peqGrid->addWidget(hdr1, 0, 0);
            peqGrid->addWidget(hdr2, 0, 1);
            peqGrid->addWidget(hdr3, 0, 2);
            peqGrid->addWidget(hdr4, 0, 3);

            auto addRow = [this, peqGroup, peqGrid](int r, const QString& name, double initialF, double initialG,
                                                    double initialQ, std::function<void(PipelineStage*, double)> setF,
                                                    std::function<void(PipelineStage*, double)> setG,
                                                    std::function<void(PipelineStage*, double)> setQ) {
                auto nameLbl = new QLabel(name, peqGroup);
                peqGrid->addWidget(nameLbl, r, 0);

                auto fSpin = new QDoubleSpinBox(peqGroup);
                fSpin->setRange(20.0, 20000.0);
                fSpin->setSingleStep(10.0);
                fSpin->setDecimals(1);
                fSpin->setValue(initialF);
                fSpin->setSuffix(" Hz");
                connect(fSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, setF](double val) {
                    auto st = currentStage();
                    if (!st)
                        return;
                    setF(st, val);
                    applyConfig();
                });
                nameLbl->setBuddy(fSpin);
                peqGrid->addWidget(fSpin, r, 1);

                auto gSpin = new QDoubleSpinBox(peqGroup);
                gSpin->setRange(-40.0, 40.0);
                gSpin->setSingleStep(0.5);
                gSpin->setDecimals(1);
                gSpin->setValue(initialG);
                gSpin->setSuffix(" dB");
                connect(gSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, setG](double val) {
                    auto st = currentStage();
                    if (!st)
                        return;
                    setG(st, val);
                    applyConfig();
                });
                peqGrid->addWidget(gSpin, r, 2);

                auto qSpin = new QDoubleSpinBox(peqGroup);
                qSpin->setRange(0.05, 100.0);
                qSpin->setSingleStep(0.05);
                qSpin->setDecimals(3);
                qSpin->setValue(initialQ);
                connect(qSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, setQ](double val) {
                    auto st = currentStage();
                    if (!st)
                        return;
                    setQ(st, val);
                    applyConfig();
                });
                peqGrid->addWidget(qSpin, r, 3);
            };

            addRow(
                1, "Low Shelf", stage.peqFls, stage.peqGls, stage.peqQls,
                [](PipelineStage* st, double v) { st->peqFls = v; },
                [](PipelineStage* st, double v) { st->peqGls = v; },
                [](PipelineStage* st, double v) { st->peqQls = v; });
            addRow(
                2, "PEQ 1", stage.peqF1, stage.peqG1, stage.peqQ1, [](PipelineStage* st, double v) { st->peqF1 = v; },
                [](PipelineStage* st, double v) { st->peqG1 = v; }, [](PipelineStage* st, double v) { st->peqQ1 = v; });
            addRow(
                3, "PEQ 2", stage.peqF2, stage.peqG2, stage.peqQ2, [](PipelineStage* st, double v) { st->peqF2 = v; },
                [](PipelineStage* st, double v) { st->peqG2 = v; }, [](PipelineStage* st, double v) { st->peqQ2 = v; });
            addRow(
                4, "PEQ 3", stage.peqF3, stage.peqG3, stage.peqQ3, [](PipelineStage* st, double v) { st->peqF3 = v; },
                [](PipelineStage* st, double v) { st->peqG3 = v; }, [](PipelineStage* st, double v) { st->peqQ3 = v; });
            addRow(
                5, "High Shelf", stage.peqFhs, stage.peqGhs, stage.peqQhs,
                [](PipelineStage* st, double v) { st->peqFhs = v; },
                [](PipelineStage* st, double v) { st->peqGhs = v; },
                [](PipelineStage* st, double v) { st->peqQhs = v; });

            containerLayout->addWidget(peqGroup);
        }
        break;
    }

    case StageType::GraphicEQ: {
        auto geqGroup = new QGroupBox("Graphic Equalizer Settings", m_optionsContainer);
        auto geqVBox = new QVBoxLayout(geqGroup);
        geqVBox->setSpacing(12);

        auto geqForm = new QFormLayout();
        geqForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto rangeWidget = new QWidget(geqGroup);
        auto rangeHBox = new QHBoxLayout(rangeWidget);
        rangeHBox->setContentsMargins(0, 0, 0, 0);
        rangeHBox->setSpacing(8);

        auto minEdit = new QLineEdit(QString::number(stage.graphicEQFreqMin), rangeWidget);
        minEdit->setFixedWidth(80);
        connect(minEdit, &QLineEdit::editingFinished, [this, &stage, minEdit]() {
            stage.graphicEQFreqMin = minEdit->text().toDouble();
            applyConfig();
            refreshUi();
        });
        rangeHBox->addWidget(minEdit);
        rangeHBox->addWidget(new QLabel("to", rangeWidget));
        auto maxEdit = new QLineEdit(QString::number(stage.graphicEQFreqMax), rangeWidget);
        maxEdit->setFixedWidth(80);
        connect(maxEdit, &QLineEdit::editingFinished, [this, &stage, maxEdit]() {
            stage.graphicEQFreqMax = maxEdit->text().toDouble();
            applyConfig();
            refreshUi();
        });
        rangeHBox->addWidget(maxEdit);
        rangeHBox->addWidget(new QLabel("Hz", rangeWidget));
        rangeHBox->addStretch();

        auto rangeLbl = new QLabel("Frequency &Range:", geqGroup);
        rangeLbl->setBuddy(minEdit);
        geqForm->addRow(rangeLbl, rangeWidget);

        auto spinBands = new QSpinBox(geqGroup);
        spinBands->setRange(2, 64);
        spinBands->setValue(stage.graphicEQBandCount);
        connect(spinBands, QOverload<int>::of(&QSpinBox::valueChanged), [this, &stage](int val) {
            stage.graphicEQBandCount = val;
            if (static_cast<int>(stage.graphicEQGains.size()) != val) {
                stage.graphicEQGains.resize(val, 0.0);
            }
            applyConfig();
            refreshUi();
        });
        addWidgetRow(geqForm, "&Bands:", spinBands, geqGroup);

        geqVBox->addLayout(geqForm);

        // Scrollable Slider Bank
        auto scrollBank = new QScrollArea(geqGroup);
        scrollBank->setWidgetResizable(true);
        scrollBank->setFixedHeight(240);

        auto bankContainer = new QWidget(scrollBank);
        auto bankLayout = new QHBoxLayout(bankContainer);
        bankLayout->setSpacing(14);
        bankLayout->setContentsMargins(4, 4, 4, 4);

        int totalBands = stage.graphicEQBandCount;
        if (static_cast<int>(stage.graphicEQGains.size()) != totalBands) {
            stage.graphicEQGains.resize(totalBands, 0.0);
        }

        auto bandFrequency = [](int index, int total, double fMin, double fMax) -> double {
            if (total <= 1)
                return fMin;
            double ratio = fMax / fMin;
            double exponent = static_cast<double>(index) / (total - 1);
            return fMin * std::pow(ratio, exponent);
        };

        auto freqLabelText = [](double hz) -> QString {
            if (hz >= 1000.0) {
                double khz = hz / 1000.0;
                if (khz == std::floor(khz))
                    return QString("%1k").arg(static_cast<int>(khz));
                else
                    return QString("%1k").arg(khz, 0, 'f', 1);
            } else {
                if (hz == std::floor(hz))
                    return QString("%1").arg(static_cast<int>(hz));
                else
                    return QString("%1").arg(hz, 0, 'f', 1);
            }
        };

        for (int b = 0; b < totalBands; ++b) {
            double freq = bandFrequency(b, totalBands, stage.graphicEQFreqMin, stage.graphicEQFreqMax);
            QString fText = freqLabelText(freq);

            auto bVBox = new QVBoxLayout();
            bVBox->setSpacing(8);
            bVBox->setAlignment(Qt::AlignCenter);

            auto gainValLbl = new QLabel(QString("%1").arg(stage.graphicEQGains[b], 0, 'f', 1), bankContainer);
            gainValLbl->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            gainValLbl->setFixedWidth(35);
            gainValLbl->setAlignment(Qt::AlignCenter);
            bVBox->addWidget(gainValLbl);

            auto slider = new VSliderWidget(stage.graphicEQGains[b], -40.0, 40.0, bankContainer);
            connect(slider, &VSliderWidget::valueChanged, [this, &stage, b, gainValLbl](double val) {
                stage.graphicEQGains[b] = val;
                gainValLbl->setText(QString("%1").arg(val, 0, 'f', 1));
                applyConfig();
            });
            bVBox->addWidget(slider, 0, Qt::AlignCenter);

            auto fLbl = new RotatedLabel(fText, bankContainer);
            bVBox->addWidget(fLbl, 0, Qt::AlignCenter);

            bankLayout->addLayout(bVBox);
        }

        scrollBank->setWidget(bankContainer);
        geqVBox->addWidget(scrollBank);

        auto resetGainsBtn = new QPushButton("Reset All to 0 dB", geqGroup);
        connect(resetGainsBtn, &QPushButton::clicked, [this, &stage]() {
            stage.graphicEQGains.assign(stage.graphicEQBandCount, 0.0);
            applyConfig();
            refreshUi();
        });
        geqVBox->addWidget(resetGainsBtn, 0, Qt::AlignLeft);

        containerLayout->addWidget(geqGroup);
        break;
    }

    default:
        break;
    }
    }

    containerLayout->addStretch();
}
