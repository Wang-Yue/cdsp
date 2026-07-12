#include "ui/StageDetailView.h"

#include "ui/StyleTheme.h"

#include <QButtonGroup>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <cmath>

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
    painter.setBrush(QColor(200, 200, 200, 100));
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
        painter.setBrush(QColor(0, 122, 255));
        painter.drawRoundedRect(centerX - trackW / 2, activeTop, trackW, activeHeight, trackW / 2, trackW / 2);
    }

    // Center 0 dB tick line (drawn over active fill track for visibility)
    painter.setPen(QPen(QColor(140, 140, 140), 1));
    painter.drawLine(centerX - 6, centerY, centerX + 6, centerY);

    // Knob Circle
    painter.setBrush(Qt::white);
    painter.setPen(QPen(QColor(0, 122, 255), 1.5));
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
    mainLayout->setContentsMargins(24, 24, 24, 24);
    mainLayout->setSpacing(20);

    // Header Toolbar
    auto headerLayout = new QHBoxLayout();
    m_nameEdit = new QLineEdit(container);
    m_nameEdit->setFont(QFont("sans-serif", 14, QFont::Bold));
    connect(m_nameEdit, &QLineEdit::editingFinished, [this]() {
        if (m_stageIndex < m_pipeline->stages.size()) {
            m_pipeline->stages[m_stageIndex].name = m_nameEdit->text().toStdString();
            applyConfig();
        }
    });
    headerLayout->addWidget(m_nameEdit);

    headerLayout->addStretch();

    m_enabledCheck = new QCheckBox("Enabled", container);
    connect(m_enabledCheck, &QCheckBox::toggled, [this](bool checked) {
        if (m_stageIndex < m_pipeline->stages.size()) {
            m_pipeline->stages[m_stageIndex].isEnabled = checked;
            if (m_optionsContainer) {
                m_optionsContainer->setEnabled(true);
                if (!checked) {
                    auto opacityEffect = new QGraphicsOpacityEffect(m_optionsContainer);
                    opacityEffect->setOpacity(0.5);
                    m_optionsContainer->setGraphicsEffect(opacityEffect);
                } else {
                    m_optionsContainer->setGraphicsEffect(nullptr);
                }
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
    if (m_isBuildingUi)
        return;
    m_isLocalEditing = true;
    m_pipeline->save();
    emit m_pipeline->pipelineChanged();
    m_dspController->applyConfig();
    m_isLocalEditing = false;
}

PipelineStage* StageDetailView::currentStage() const {
    if (m_pipeline && m_stageIndex < m_pipeline->stages.size()) {
        return &m_pipeline->stages[m_stageIndex];
    }
    return nullptr;
}

void StageDetailView::refreshUi() {
    if (m_isLocalEditing)
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

void StageDetailView::buildStageOptionsUi() {
    auto containerLayout = qobject_cast<QVBoxLayout*>(m_optionsContainer->layout());
    QLayoutItem* item;
    while ((item = containerLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    if (m_stageIndex >= m_pipeline->stages.size())
        return;
    auto& stage = m_pipeline->stages[m_stageIndex];

    m_optionsContainer->setEnabled(true);
    if (!stage.isEnabled) {
        auto opacityEffect = new QGraphicsOpacityEffect(m_optionsContainer);
        opacityEffect->setOpacity(0.5);
        m_optionsContainer->setGraphicsEffect(opacityEffect);
    } else {
        m_optionsContainer->setGraphicsEffect(nullptr);
    }

    int hwChannels =
        (m_dspController && m_dspController->devices()) ? m_dspController->devices()->captureConfig.channels : 8;
    int incomingChannels = m_pipeline ? m_pipeline->channelCountBeforeStage(m_stageIndex, hwChannels) : hwChannels;
    if (incomingChannels < 1)
        incomingChannels = 2;

    // 1. Channel Selector Card (Unified for all stages except Matrix Mixer)
    if (stage.type != StageType::MatrixMixer) {
        auto chanGroup = new QGroupBox("Target Channels", m_optionsContainer);
        auto chanLayout = new QVBoxLayout(chanGroup);

        if (stage.type == StageType::Balance || stage.type == StageType::Width || stage.type == StageType::MSProc ||
            stage.type == StageType::Crossfeed || stage.type == StageType::RACE ||
            stage.type == StageType::SplitWidth) {
            auto pairBox = new QHBoxLayout();

            auto leftBox = new QVBoxLayout();
            leftBox->addWidget(new QLabel("Left Input", chanGroup));
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
            leftBox->addWidget(leftCombo);
            pairBox->addLayout(leftBox);

            auto rightBox = new QVBoxLayout();
            rightBox->addWidget(new QLabel("Right Input", chanGroup));
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
            rightBox->addWidget(rightCombo);
            pairBox->addLayout(rightBox);

            pairBox->addStretch();
            chanLayout->addLayout(pairBox);

            auto descLbl = new QLabel("This stereo stage will process the selected Left and Right channels. All other "
                                      "channels will pass through unaffected.",
                                      chanGroup);
            descLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
            chanLayout->addWidget(descLbl);
        } else {
            auto pillsLayout = new QHBoxLayout();
            for (int c = 0; c < incomingChannels; ++c) {
                auto btn = new QPushButton(QString::number(c + 1), chanGroup);
                btn->setFixedWidth(36);
                btn->setCheckable(true);
                bool isSelected = std::find(stage.channels.begin(), stage.channels.end(), c) != stage.channels.end();
                btn->setChecked(isSelected);

                auto updateBtnStyle = [btn](bool checked) {
                    if (checked) {
                        btn->setStyleSheet("background-color: #007aff; color: white; font-weight: bold; border-radius: "
                                           "4px; border: none;");
                    } else {
                        btn->setStyleSheet("background-color: rgba(142, 142, 147, 0.15); color: palette(text); "
                                           "font-weight: bold; border-radius: 4px; border: none;");
                    }
                };
                updateBtnStyle(isSelected);

                connect(btn, &QPushButton::clicked, [this, c, btn, updateBtnStyle]() {
                    auto st = currentStage();
                    if (!st)
                        return;
                    auto it = std::find(st->channels.begin(), st->channels.end(), c);
                    if (it != st->channels.end()) {
                        if (st->channels.size() > 1) {
                            st->channels.erase(it);
                            btn->setChecked(false);
                        } else {
                            btn->setChecked(true);
                        }
                    } else {
                        st->channels.push_back(c);
                        btn->setChecked(true);
                    }
                    updateBtnStyle(btn->isChecked());
                    applyConfig();
                });
                pillsLayout->addWidget(btn);
            }
            pillsLayout->addStretch();
            chanLayout->addLayout(pillsLayout);
        }
        containerLayout->addWidget(chanGroup);
    }

    // 2. Stage Options Panels
    switch (stage.type) {
    case StageType::Balance: {
        auto balGroup = new QGroupBox("Balance", m_optionsContainer);
        auto balVBox = new QVBoxLayout(balGroup);

        auto sliderHBox = new QHBoxLayout();
        sliderHBox->addWidget(new QLabel("L", balGroup));
        auto balSlider = new QSlider(Qt::Horizontal, balGroup);
        balSlider->setRange(-100, 100);
        balSlider->setValue(static_cast<int>(stage.balancePosition * 100.0));
        sliderHBox->addWidget(balSlider);
        sliderHBox->addWidget(new QLabel("R", balGroup));
        balVBox->addLayout(sliderHBox);

        auto infoHBox = new QHBoxLayout();
        auto leftLbl = new QLabel(QString("Left: %1%").arg(stage.balanceLeftPercent()), balGroup);
        infoHBox->addWidget(leftLbl);

        auto centerBtn = new QPushButton("Center", balGroup);
        connect(centerBtn, &QPushButton::clicked, [this, balSlider]() {
            auto st = currentStage();
            if (!st)
                return;
            st->balancePosition = 0.0;
            balSlider->setValue(0);
            applyConfig();
            refreshUi();
        });
        infoHBox->addWidget(centerBtn);

        auto rightLbl = new QLabel(QString("Right: %1%").arg(stage.balanceRightPercent()), balGroup);
        rightLbl->setAlignment(Qt::AlignRight);
        infoHBox->addWidget(rightLbl);

        balVBox->addLayout(infoHBox);

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
        auto widthVBox = new QVBoxLayout(widthGroup);

        auto sliderHBox = new QHBoxLayout();
        sliderHBox->addWidget(new QLabel("Swapped", widthGroup));
        auto widthSlider = new QSlider(Qt::Horizontal, widthGroup);
        widthSlider->setRange(-100, 200);
        widthSlider->setValue(static_cast<int>(stage.widthAmount * 100.0));
        sliderHBox->addWidget(widthSlider);
        sliderHBox->addWidget(new QLabel("Wide", widthGroup));
        widthVBox->addLayout(sliderHBox);

        auto infoHBox = new QHBoxLayout();
        auto pctLbl = new QLabel(QString("%1%").arg(stage.widthPercent()), widthGroup);
        pctLbl->setFont(QFont("sans-serif", 12, QFont::Bold));
        infoHBox->addWidget(pctLbl);

        infoHBox->addStretch();

        auto btnNeg100 = new QPushButton("-100%", widthGroup);
        connect(btnNeg100, &QPushButton::clicked, [this, &stage]() {
            stage.widthAmount = -1.0;
            applyConfig();
            refreshUi();
        });
        infoHBox->addWidget(btnNeg100);

        auto btnMono = new QPushButton("Mono", widthGroup);
        connect(btnMono, &QPushButton::clicked, [this, &stage]() {
            stage.widthAmount = 0.0;
            applyConfig();
            refreshUi();
        });
        infoHBox->addWidget(btnMono);

        auto btn100 = new QPushButton("100%", widthGroup);
        connect(btn100, &QPushButton::clicked, [this, &stage]() {
            stage.widthAmount = 1.0;
            applyConfig();
            refreshUi();
        });
        infoHBox->addWidget(btn100);

        widthVBox->addLayout(infoHBox);

        auto descLbl = new QLabel(QString::fromStdString(stage.widthDescription()), widthGroup);
        descLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        widthVBox->addWidget(descLbl);

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
        msLbl->setStyleSheet("color: #8e8e93; font-size: 12px;");
        msBox->addWidget(msLbl);
        containerLayout->addWidget(msGroup);
        break;
    }

    case StageType::PhaseInvert: {
        auto piGroup = new QGroupBox("Phase Inversion", m_optionsContainer);
        auto piBox = new QVBoxLayout(piGroup);
        auto piLbl = new QLabel("Inverts the phase (polarity) of all selected channels.", piGroup);
        piLbl->setStyleSheet("color: #8e8e93; font-size: 12px;");
        piBox->addWidget(piLbl);
        containerLayout->addWidget(piGroup);
        break;
    }

    case StageType::Crossfeed: {
        auto presetGroup = new QGroupBox("Preset", m_optionsContainer);
        auto presetVBox = new QVBoxLayout(presetGroup);

        auto levelHBox = new QHBoxLayout();
        levelHBox->addWidget(new QLabel("Level:", presetGroup));

        auto levelCombo = new QComboBox(presetGroup);
        levelCombo->addItems({"L1", "L2", "L3", "L4", "L5"});
        levelCombo->setCurrentIndex(static_cast<int>(stage.crossfeedLevel) - 1);
        levelCombo->setEnabled(!stage.cxCustomEnabled);
        connect(levelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.crossfeedLevel = static_cast<CrossfeedLevel>(idx + 1);
            applyConfig();
            refreshUi();
        });
        levelHBox->addWidget(levelCombo);
        levelHBox->addStretch();
        presetVBox->addLayout(levelHBox);

        auto cxParams = stage.activeCrossfeedParams();
        auto detailLbl =
            new QLabel(QString("Fc = %1 Hz, Level = %2 dB — %3")
                           .arg(stage.cxCustomEnabled ? stage.cxFc
                                                      : (stage.crossfeedLevel == CrossfeedLevel::L1 ||
                                                                 stage.crossfeedLevel == CrossfeedLevel::L2
                                                             ? 650
                                                             : 700))
                           .arg(stage.cxCustomEnabled
                                    ? stage.cxDb
                                    : (stage.crossfeedLevel == CrossfeedLevel::L1
                                           ? 13.5
                                           : (stage.crossfeedLevel == CrossfeedLevel::L2
                                                  ? 9.5
                                                  : (stage.crossfeedLevel == CrossfeedLevel::L3
                                                         ? 6.0
                                                         : (stage.crossfeedLevel == CrossfeedLevel::L4 ? 4.5 : 3.0)))))
                           .arg(QString::fromStdString(crossfeedLevelDescription(stage.crossfeedLevel))),
                       presetGroup);
        detailLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        presetVBox->addWidget(detailLbl);

        containerLayout->addWidget(presetGroup);

        auto customGroup = new QGroupBox("Custom Parameters", m_optionsContainer);
        auto customVBox = new QVBoxLayout(customGroup);

        auto customToggle = new QCheckBox("Custom Parameters", customGroup);
        customToggle->setChecked(stage.cxCustomEnabled);
        connect(customToggle, &QCheckBox::toggled, [this, &stage](bool checked) {
            stage.cxCustomEnabled = checked;
            if (checked) {
                double fc = 700.0;
                double db = 6.0;
                switch (stage.crossfeedLevel) {
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
                stage.cxFc = fc;
                stage.cxDb = db;
            }
            applyConfig();
            refreshUi();
        });
        customVBox->addWidget(customToggle);

        if (stage.cxCustomEnabled) {
            auto formLayout = new QFormLayout();
            formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

            auto fcSlider = new QSlider(Qt::Horizontal, customGroup);
            fcSlider->setRange(300, 1200);
            fcSlider->setValue(static_cast<int>(stage.cxFc));
            auto fcLbl = new QLabel(QString("%1 Hz").arg(static_cast<int>(stage.cxFc)), customGroup);
            fcLbl->setFixedWidth(65);
            fcLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            fcLbl->setFont(QFont("monospace", 11));
            connect(fcSlider, &QSlider::valueChanged, [this, &stage, fcLbl](int val) {
                stage.cxFc = val;
                fcLbl->setText(QString("%1 Hz").arg(val));
                applyConfig();
            });
            auto fcBox = new QHBoxLayout();
            fcBox->addWidget(fcSlider);
            fcBox->addWidget(fcLbl);
            formLayout->addRow("Fc (Hz):", fcBox);

            auto dbSlider = new QSlider(Qt::Horizontal, customGroup);
            dbSlider->setRange(10, 200);
            dbSlider->setValue(static_cast<int>(stage.cxDb * 10.0));
            auto dbLbl = new QLabel(QString("%1 dB").arg(stage.cxDb, 0, 'f', 1), customGroup);
            dbLbl->setFixedWidth(65);
            dbLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            dbLbl->setFont(QFont("monospace", 11));
            connect(dbSlider, &QSlider::valueChanged, [this, &stage, dbLbl](int val) {
                stage.cxDb = val / 10.0;
                dbLbl->setText(QString("%1 dB").arg(stage.cxDb, 0, 'f', 1));
                applyConfig();
            });
            auto dbBox = new QHBoxLayout();
            dbBox->addWidget(dbSlider);
            dbBox->addWidget(dbLbl);
            formLayout->addRow("Level (dB):", dbBox);

            customVBox->addLayout(formLayout);
        }
        containerLayout->addWidget(customGroup);

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
        auto crossVBox = new QVBoxLayout(crossGroup);

        auto crossHBox = new QHBoxLayout();
        crossHBox->addWidget(new QLabel("Crossover", crossGroup));
        auto crossSlider = new QSlider(Qt::Horizontal, crossGroup);
        crossSlider->setRange(40, 1000);
        crossSlider->setSingleStep(5);
        crossSlider->setValue(static_cast<int>(stage.splitWidthCrossover));
        crossHBox->addWidget(crossSlider);

        auto crossLbl = new QLabel(QString("%1 Hz").arg(static_cast<int>(stage.splitWidthCrossover)), crossGroup);
        crossLbl->setFont(QFont("sans-serif", 10, QFont::Bold));
        crossHBox->addWidget(crossLbl);
        crossVBox->addLayout(crossHBox);

        auto crossDesc = new QLabel("Frequencies below this crossover will remain centered (mono-summed), while "
                                    "frequencies above this point will be widened.",
                                    crossGroup);
        crossDesc->setStyleSheet("color: #8e8e93; font-size: 11px;");
        crossVBox->addWidget(crossDesc);

        connect(crossSlider, &QSlider::valueChanged, [this, &stage, crossLbl](int val) {
            stage.splitWidthCrossover = val;
            crossLbl->setText(QString("%1 Hz").arg(val));
            applyConfig();
        });
        containerLayout->addWidget(crossGroup);

        auto highGroup = new QGroupBox("High Band Stereo Width", m_optionsContainer);
        auto highVBox = new QVBoxLayout(highGroup);

        auto highSliderHBox = new QHBoxLayout();
        highSliderHBox->addWidget(new QLabel("Mono", highGroup));
        auto highSlider = new QSlider(Qt::Horizontal, highGroup);
        highSlider->setRange(0, 200);
        highSlider->setValue(static_cast<int>(stage.splitWidthAmount * 100.0));
        highSliderHBox->addWidget(highSlider);
        highSliderHBox->addWidget(new QLabel("Wide", highGroup));
        highVBox->addLayout(highSliderHBox);

        auto highInfoHBox = new QHBoxLayout();
        auto highPctLbl = new QLabel(QString("%1%").arg(static_cast<int>(stage.splitWidthAmount * 100.0)), highGroup);
        highPctLbl->setFont(QFont("sans-serif", 12, QFont::Bold));
        highInfoHBox->addWidget(highPctLbl);

        highInfoHBox->addStretch();

        auto btnMono = new QPushButton("Mono", highGroup);
        connect(btnMono, &QPushButton::clicked, [this, &stage]() {
            stage.splitWidthAmount = 0.0;
            applyConfig();
            refreshUi();
        });
        highInfoHBox->addWidget(btnMono);

        auto btn100 = new QPushButton("100% (Normal)", highGroup);
        connect(btn100, &QPushButton::clicked, [this, &stage]() {
            stage.splitWidthAmount = 1.0;
            applyConfig();
            refreshUi();
        });
        highInfoHBox->addWidget(btn100);

        auto btn150 = new QPushButton("150% (Wide)", highGroup);
        connect(btn150, &QPushButton::clicked, [this, &stage]() {
            stage.splitWidthAmount = 1.5;
            applyConfig();
            refreshUi();
        });
        highInfoHBox->addWidget(btn150);

        highVBox->addLayout(highInfoHBox);

        auto highDesc = new QLabel("Adjusts the stereo width of frequencies above the crossover point. 0% is full "
                                   "mono, 100% is normal stereo, and 150%+ is enhanced width.",
                                   highGroup);
        highDesc->setStyleSheet("color: #8e8e93; font-size: 11px;");
        highVBox->addWidget(highDesc);

        connect(highSlider, &QSlider::valueChanged, [this, &stage, highPctLbl](int val) {
            stage.splitWidthAmount = val / 100.0;
            highPctLbl->setText(QString("%1%").arg(val));
            applyConfig();
        });
        containerLayout->addWidget(highGroup);
        break;
    }

    case StageType::EQ: {
        auto eqGroup = new QGroupBox("EQ Preset", m_optionsContainer);
        auto eqVBox = new QVBoxLayout(eqGroup);

        auto eqHBox = new QHBoxLayout();
        eqHBox->addWidget(new QLabel("Preset:", eqGroup));

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
        eqHBox->addWidget(eqCombo);
        eqHBox->addStretch();
        eqVBox->addLayout(eqHBox);

        if (stage.eqPresetId.has_value()) {
            auto it = std::find_if(m_pipeline->eqPresets.begin(), m_pipeline->eqPresets.end(),
                                   [&stage](const EQPreset& p) { return p.id == stage.eqPresetId.value(); });
            if (it != m_pipeline->eqPresets.end()) {
                auto preampLbl = new QLabel(
                    QString("Preamp Gain: %1%2 dB").arg(it->preampGain >= 0 ? "+" : "").arg(it->preampGain, 0, 'f', 1),
                    eqGroup);
                eqVBox->addWidget(preampLbl);

                auto diagWidget = new EQDiagramWidget(eqGroup);
                diagWidget->setPreset(*it);
                diagWidget->setFixedHeight(150);
                eqVBox->addWidget(diagWidget);
            }
        }
        containerLayout->addWidget(eqGroup);
        break;
    }

    case StageType::Convolution: {
        auto convGroup = new QGroupBox("Convolution Preset", m_optionsContainer);
        auto convVBox = new QVBoxLayout(convGroup);

        auto convHBox = new QHBoxLayout();
        convHBox->addWidget(new QLabel("Preset:", convGroup));

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
        convHBox->addWidget(convCombo);
        convHBox->addStretch();
        convVBox->addLayout(convHBox);

        if (stage.convPresetId.has_value()) {
            auto it = std::find_if(m_pipeline->convPresets.begin(), m_pipeline->convPresets.end(),
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
                metaLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
                convVBox->addWidget(metaLbl);

                std::string irPath = it->irPath(effectiveRate);
                if (!irPath.empty()) {
                    auto plot = new ConvolutionIRPlot(convGroup);
                    plot->setIRPath(irPath);
                    plot->setFixedHeight(110);
                    convVBox->addWidget(plot);
                }
            }
        }
        containerLayout->addWidget(convGroup);
        break;
    }

    case StageType::Loudness: {
        auto loudGroup = new QGroupBox("Loudness Compensation", m_optionsContainer);
        auto formLayout = new QFormLayout(loudGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto refSlider = new QSlider(Qt::Horizontal, loudGroup);
        refSlider->setRange(-100, 20);
        refSlider->setValue(static_cast<int>(stage.loudnessReference));
        auto refLbl = new QLabel(QString("%1 dB").arg(static_cast<int>(stage.loudnessReference)), loudGroup);
        refLbl->setFixedWidth(65);
        refLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        refLbl->setFont(QFont("monospace", 11));
        connect(refSlider, &QSlider::valueChanged, [this, &stage, refLbl](int val) {
            stage.loudnessReference = val;
            refLbl->setText(QString("%1 dB").arg(val));
            applyConfig();
        });
        auto refBox = new QHBoxLayout();
        refBox->addWidget(refSlider);
        refBox->addWidget(refLbl);
        formLayout->addRow("Reference Level:", refBox);

        auto lowSlider = new QSlider(Qt::Horizontal, loudGroup);
        lowSlider->setRange(0, 40);
        lowSlider->setValue(static_cast<int>(stage.loudnessLowBoost * 2.0));
        auto lowLbl = new QLabel(QString("%1 dB").arg(stage.loudnessLowBoost, 0, 'f', 1), loudGroup);
        lowLbl->setFixedWidth(65);
        lowLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lowLbl->setFont(QFont("monospace", 11));
        connect(lowSlider, &QSlider::valueChanged, [this, &stage, lowLbl](int val) {
            stage.loudnessLowBoost = val / 2.0;
            lowLbl->setText(QString("%1 dB").arg(stage.loudnessLowBoost, 0, 'f', 1));
            applyConfig();
        });
        auto lowBox = new QHBoxLayout();
        lowBox->addWidget(lowSlider);
        lowBox->addWidget(lowLbl);
        formLayout->addRow("Low Boost:", lowBox);

        auto highSlider = new QSlider(Qt::Horizontal, loudGroup);
        highSlider->setRange(0, 40);
        highSlider->setValue(static_cast<int>(stage.loudnessHighBoost * 2.0));
        auto highLbl = new QLabel(QString("%1 dB").arg(stage.loudnessHighBoost, 0, 'f', 1), loudGroup);
        highLbl->setFixedWidth(65);
        highLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        highLbl->setFont(QFont("monospace", 11));
        connect(highSlider, &QSlider::valueChanged, [this, &stage, highLbl](int val) {
            stage.loudnessHighBoost = val / 2.0;
            highLbl->setText(QString("%1 dB").arg(stage.loudnessHighBoost, 0, 'f', 1));
            applyConfig();
        });
        auto highBox = new QHBoxLayout();
        highBox->addWidget(highSlider);
        highBox->addWidget(highLbl);
        formLayout->addRow("High Boost:", highBox);

        auto faderCombo = new QComboBox(loudGroup);
        faderCombo->addItems({"Main", "Aux 1", "Aux 2", "Aux 3", "Aux 4"});
        faderCombo->setCurrentIndex(static_cast<int>(stage.loudnessFader));
        connect(faderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.loudnessFader = static_cast<Fader>(idx);
            applyConfig();
        });
        formLayout->addRow("Fader:", faderCombo);

        auto attChk = new QCheckBox("Attenuate Mid (instead of boosting extremes)", loudGroup);
        attChk->setChecked(stage.loudnessAttenuateMid);
        connect(attChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.loudnessAttenuateMid = chk;
            applyConfig();
        });
        formLayout->addRow("", attChk);

        containerLayout->addWidget(loudGroup);
        break;
    }

    case StageType::Emphasis: {
        auto empGroup = new QGroupBox("Emphasis", m_optionsContainer);
        auto empVBox = new QVBoxLayout(empGroup);

        auto modeHBox = new QHBoxLayout();
        modeHBox->addWidget(new QLabel("Mode:", empGroup));

        auto modeCombo = new QComboBox(empGroup);
        modeCombo->addItems({"De-Emphasis", "Pre-Emphasis"});
        modeCombo->setCurrentIndex(stage.emphasisMode == EmphasisMode::PreEmphasis ? 1 : 0);
        connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.emphasisMode = (idx == 1) ? EmphasisMode::PreEmphasis : EmphasisMode::DeEmphasis;
            applyConfig();
            refreshUi();
        });
        modeHBox->addWidget(modeCombo);
        modeHBox->addStretch();
        empVBox->addLayout(modeHBox);

        auto descLbl = new QLabel(QString::fromStdString(emphasisModeDescription(stage.emphasisMode)), empGroup);
        descLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        empVBox->addWidget(descLbl);

        containerLayout->addWidget(empGroup);
        break;
    }

    case StageType::DCProtection: {
        auto dcpGroup = new QGroupBox("DC Protection", m_optionsContainer);
        auto dcpVBox = new QVBoxLayout(dcpGroup);
        auto dcpLbl = new QLabel(
            "First-order highpass at 7 Hz — removes DC offset and subsonic content on all selected channels.",
            dcpGroup);
        dcpLbl->setStyleSheet("color: #8e8e93; font-size: 12px;");
        dcpVBox->addWidget(dcpLbl);
        containerLayout->addWidget(dcpGroup);
        break;
    }

    case StageType::Gain: {
        auto gainGroup = new QGroupBox("Gain / Mute Settings", m_optionsContainer);
        auto gainVBox = new QVBoxLayout(gainGroup);

        auto sliderHBox = new QHBoxLayout();
        sliderHBox->addWidget(new QLabel("Gain:", gainGroup));

        auto gainSlider = new QSlider(Qt::Horizontal, gainGroup);
        gainSlider->setRange(-1500, 1500);
        gainSlider->setValue(static_cast<int>(stage.gainValue * 10.0));
        sliderHBox->addWidget(gainSlider);

        auto gainLbl = new QLabel(
            QString("%1%2 dB").arg(stage.gainValue >= 0 ? "+" : "").arg(stage.gainValue, 0, 'f', 1), gainGroup);
        sliderHBox->addWidget(gainLbl);

        auto resetBtn = new QPushButton("Reset", gainGroup);
        connect(resetBtn, &QPushButton::clicked, [this, &stage]() {
            stage.gainValue = 0.0;
            applyConfig();
            refreshUi();
        });
        sliderHBox->addWidget(resetBtn);

        gainVBox->addLayout(sliderHBox);

        auto optionsHBox = new QHBoxLayout();
        auto invChk = new QCheckBox("Invert Polarity", gainGroup);
        invChk->setChecked(stage.gainInverted);
        connect(invChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.gainInverted = chk;
            applyConfig();
        });
        optionsHBox->addWidget(invChk);

        auto muteChk = new QCheckBox("Mute", gainGroup);
        muteChk->setChecked(stage.gainMuted);
        connect(muteChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.gainMuted = chk;
            applyConfig();
        });
        optionsHBox->addWidget(muteChk);

        optionsHBox->addStretch();
        gainVBox->addLayout(optionsHBox);

        connect(gainSlider, &QSlider::valueChanged, [this, &stage, gainLbl](int val) {
            stage.gainValue = val / 10.0;
            gainLbl->setText(QString("%1%2 dB").arg(stage.gainValue >= 0 ? "+" : "").arg(stage.gainValue, 0, 'f', 1));
            applyConfig();
        });

        containerLayout->addWidget(gainGroup);
        break;
    }

    case StageType::Delay: {
        auto delayGroup = new QGroupBox("Delay / Time Alignment", m_optionsContainer);
        auto formLayout = new QFormLayout(delayGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto unitCombo = new QComboBox(delayGroup);
        unitCombo->addItems({"Milliseconds (ms)", "Microseconds (μs)", "Samples", "Millimeters (mm)"});
        unitCombo->setCurrentIndex(static_cast<int>(stage.delayUnit));
        connect(unitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.delayUnit = static_cast<DelayUnit>(idx);
            applyConfig();
            refreshUi();
        });
        formLayout->addRow("Unit:", unitCombo);

        double maxVal = (stage.delayUnit == DelayUnit::samples)
                            ? 96000.0
                            : ((stage.delayUnit == DelayUnit::us) ? 1000000.0 : 1000.0);
        double stepVal = (stage.delayUnit == DelayUnit::samples) ? (stage.delaySubsample ? 0.01 : 1.0) : 0.1;

        auto delaySlider = new QSlider(Qt::Horizontal, delayGroup);
        int stepsCount = static_cast<int>(maxVal / stepVal);
        delaySlider->setRange(0, stepsCount);
        delaySlider->setValue(static_cast<int>(stage.delayValue / stepVal));

        auto delayLbl = new QLabel(QString("%1 %2")
                                       .arg(stage.delayValue, 0, 'f', 2)
                                       .arg(QString::fromStdString(delayUnitToString(stage.delayUnit))),
                                   delayGroup);
        delayLbl->setFixedWidth(80);
        delayLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        delayLbl->setFont(QFont("monospace", 11));

        auto zeroBtn = new QPushButton("Zero", delayGroup);
        connect(zeroBtn, &QPushButton::clicked, [this, &stage]() {
            stage.delayValue = 0.0;
            applyConfig();
            refreshUi();
        });

        auto sliderHBox = new QHBoxLayout();
        sliderHBox->addWidget(delaySlider);
        sliderHBox->addWidget(delayLbl);
        sliderHBox->addWidget(zeroBtn);
        formLayout->addRow("Delay:", sliderHBox);

        auto subChk = new QCheckBox("Subsample Delay (uses IIR allpass filter)", delayGroup);
        subChk->setChecked(stage.delaySubsample);
        connect(subChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.delaySubsample = chk;
            applyConfig();
            refreshUi();
        });
        formLayout->addRow("", subChk);

        connect(delaySlider, &QSlider::valueChanged, [this, &stage, delayLbl, stepVal](int val) {
            stage.delayValue = val * stepVal;
            delayLbl->setText(QString("%1 %2")
                                  .arg(stage.delayValue, 0, 'f', 2)
                                  .arg(QString::fromStdString(delayUnitToString(stage.delayUnit))));
            applyConfig();
        });

        containerLayout->addWidget(delayGroup);
        break;
    }

    case StageType::Volume: {
        auto volGroup = new QGroupBox("Volume Control", m_optionsContainer);
        auto formLayout = new QFormLayout(volGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto faderCombo = new QComboBox(volGroup);
        faderCombo->addItems({"Aux 1", "Aux 2", "Aux 3", "Aux 4"});
        faderCombo->setCurrentIndex(std::max(0, static_cast<int>(stage.volumeFader) - 1));
        connect(faderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.volumeFader = static_cast<Fader>(idx + 1);
            applyConfig();
        });
        formLayout->addRow("Fader:", faderCombo);

        auto rampSlider = new QSlider(Qt::Horizontal, volGroup);
        rampSlider->setRange(0, 40);
        rampSlider->setValue(static_cast<int>(stage.volumeRampTime / 50.0));
        auto rampLbl = new QLabel(QString("%1 ms").arg(static_cast<int>(stage.volumeRampTime)), volGroup);
        rampLbl->setFixedWidth(65);
        rampLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rampLbl->setFont(QFont("monospace", 11));
        connect(rampSlider, &QSlider::valueChanged, [this, &stage, rampLbl](int val) {
            stage.volumeRampTime = val * 50;
            rampLbl->setText(QString("%1 ms").arg(static_cast<int>(stage.volumeRampTime)));
            applyConfig();
        });
        auto rampBox = new QHBoxLayout();
        rampBox->addWidget(rampSlider);
        rampBox->addWidget(rampLbl);
        formLayout->addRow("Ramp Time:", rampBox);

        auto limitSlider = new QSlider(Qt::Horizontal, volGroup);
        limitSlider->setRange(-500, 200);
        limitSlider->setValue(static_cast<int>(stage.volumeLimit * 10.0));
        auto limitLbl = new QLabel(
            QString("%1%2 dB").arg(stage.volumeLimit >= 0 ? "+" : "").arg(stage.volumeLimit, 0, 'f', 1), volGroup);
        limitLbl->setFixedWidth(65);
        limitLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        limitLbl->setFont(QFont("monospace", 11));
        connect(limitSlider, &QSlider::valueChanged, [this, &stage, limitLbl](int val) {
            stage.volumeLimit = val / 10.0;
            limitLbl->setText(
                QString("%1%2 dB").arg(stage.volumeLimit >= 0 ? "+" : "").arg(stage.volumeLimit, 0, 'f', 1));
            applyConfig();
        });
        auto limitBox = new QHBoxLayout();
        limitBox->addWidget(limitSlider);
        limitBox->addWidget(limitLbl);
        formLayout->addRow("Limit:", limitBox);

        containerLayout->addWidget(volGroup);
        break;
    }

    case StageType::LookaheadLimiter: {
        auto limGroup = new QGroupBox("Lookahead Peak Limiter", m_optionsContainer);
        auto formLayout = new QFormLayout(limGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto limitSlider = new QSlider(Qt::Horizontal, limGroup);
        limitSlider->setRange(-300, 0);
        limitSlider->setValue(static_cast<int>(stage.lookaheadLimit * 10.0));
        auto limitLbl = new QLabel(QString("%1 dB").arg(stage.lookaheadLimit, 0, 'f', 1), limGroup);
        limitLbl->setFixedWidth(65);
        limitLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        limitLbl->setFont(QFont("monospace", 11));
        connect(limitSlider, &QSlider::valueChanged, [this, &stage, limitLbl](int val) {
            stage.lookaheadLimit = val / 10.0;
            limitLbl->setText(QString("%1 dB").arg(stage.lookaheadLimit, 0, 'f', 1));
            applyConfig();
        });
        auto limitBox = new QHBoxLayout();
        limitBox->addWidget(limitSlider);
        limitBox->addWidget(limitLbl);
        formLayout->addRow("Limit:", limitBox);

        auto attSlider = new QSlider(Qt::Horizontal, limGroup);
        attSlider->setRange(1, 10000);
        attSlider->setValue(static_cast<int>(stage.lookaheadAttack * 10.0));
        auto attLbl = new QLabel(QString("%1 ms").arg(stage.lookaheadAttack, 0, 'f', 1), limGroup);
        attLbl->setFixedWidth(65);
        attLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        attLbl->setFont(QFont("monospace", 11));
        connect(attSlider, &QSlider::valueChanged, [this, &stage, attLbl](int val) {
            stage.lookaheadAttack = val / 10.0;
            attLbl->setText(QString("%1 ms").arg(stage.lookaheadAttack, 0, 'f', 1));
            applyConfig();
        });
        auto attBox = new QHBoxLayout();
        attBox->addWidget(attSlider);
        attBox->addWidget(attLbl);
        formLayout->addRow("Attack:", attBox);

        auto relSlider = new QSlider(Qt::Horizontal, limGroup);
        relSlider->setRange(5, 1000);
        relSlider->setValue(static_cast<int>(stage.lookaheadRelease));
        auto relLbl = new QLabel(QString("%1 ms").arg(static_cast<int>(stage.lookaheadRelease)), limGroup);
        relLbl->setFixedWidth(65);
        relLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        relLbl->setFont(QFont("monospace", 11));
        connect(relSlider, &QSlider::valueChanged, [this, &stage, relLbl](int val) {
            stage.lookaheadRelease = val;
            relLbl->setText(QString("%1 ms").arg(val));
            applyConfig();
        });
        auto relBox = new QHBoxLayout();
        relBox->addWidget(relSlider);
        relBox->addWidget(relLbl);
        formLayout->addRow("Release:", relBox);

        containerLayout->addWidget(limGroup);
        break;
    }

    case StageType::Limiter: {
        auto limGroup = new QGroupBox("Peak Limiter", m_optionsContainer);
        auto formLayout = new QFormLayout(limGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto limitSlider = new QSlider(Qt::Horizontal, limGroup);
        limitSlider->setRange(-300, 0);
        limitSlider->setValue(static_cast<int>(stage.limiterLimit * 10.0));
        auto limitLbl = new QLabel(QString("%1 dB").arg(stage.limiterLimit, 0, 'f', 1), limGroup);
        limitLbl->setFixedWidth(65);
        limitLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        limitLbl->setFont(QFont("monospace", 11));
        connect(limitSlider, &QSlider::valueChanged, [this, &stage, limitLbl](int val) {
            stage.limiterLimit = val / 10.0;
            limitLbl->setText(QString("%1 dB").arg(stage.limiterLimit, 0, 'f', 1));
            applyConfig();
        });
        auto limitBox = new QHBoxLayout();
        limitBox->addWidget(limitSlider);
        limitBox->addWidget(limitLbl);
        formLayout->addRow("Limit:", limitBox);

        auto softChk = new QCheckBox("Enable Soft Clipping", limGroup);
        softChk->setChecked(stage.limiterSoftClip);
        connect(softChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.limiterSoftClip = chk;
            applyConfig();
        });
        formLayout->addRow("", softChk);

        containerLayout->addWidget(limGroup);
        break;
    }

    case StageType::MatrixMixer: {
        auto mmGroup = new QGroupBox("Matrix Mixer Configuration", m_optionsContainer);
        auto mmVBox = new QVBoxLayout(mmGroup);

        auto dimHBox = new QHBoxLayout();

        auto inBox = new QVBoxLayout();
        inBox->addWidget(new QLabel("Input Channels", mmGroup));
        auto inCombo = new QComboBox(mmGroup);
        for (int c = 1; c <= 16; ++c)
            inCombo->addItem(QString("%1 Channels").arg(c), c);
        inCombo->setCurrentIndex(stage.mixerChannelsIn - 1);
        connect(inCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, inCombo](int idx) {
            stage.mixerChannelsIn = inCombo->itemData(idx).toInt();
            applyConfig();
            refreshUi();
        });
        inBox->addWidget(inCombo);
        dimHBox->addLayout(inBox);

        auto outBox = new QVBoxLayout();
        outBox->addWidget(new QLabel("Output Channels", mmGroup));
        auto outCombo = new QComboBox(mmGroup);
        for (int c = 1; c <= 16; ++c)
            outCombo->addItem(QString("%1 Channels").arg(c), c);
        outCombo->setCurrentIndex(stage.mixerChannelsOut - 1);
        connect(outCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, outCombo](int idx) {
            stage.mixerChannelsOut = outCombo->itemData(idx).toInt();
            applyConfig();
            refreshUi();
        });
        outBox->addWidget(outCombo);
        dimHBox->addLayout(outBox);

        dimHBox->addStretch();
        mmVBox->addLayout(dimHBox);

        auto matrixGroup = new QGroupBox("Matrix Mixer Mapping", mmGroup);
        auto matrixVBox = new QVBoxLayout(matrixGroup);

        int rows = stage.mixerChannelsOut;
        int cols = stage.mixerChannelsIn;

        auto table = new QTableWidget(rows, cols, matrixGroup);
        table->horizontalHeader()->setDefaultSectionSize(95);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table->verticalHeader()->setDefaultSectionSize(80);
        table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table->setMinimumHeight(std::min(480, rows * 80 + 35));

        QStringList headers;
        for (int c = 0; c < cols; ++c)
            headers << QString("Ch %1").arg(c + 1);
        table->setHorizontalHeaderLabels(headers);

        QStringList rowLabels;
        for (int r = 0; r < rows; ++r)
            rowLabels << QString("Ch %1").arg(r + 1);
        table->setVerticalHeaderLabels(rowLabels);

        for (int r = 0; r < rows; ++r) {
            auto mapIt = std::find_if(stage.mixerMappings.begin(), stage.mixerMappings.end(),
                                      [r](const MixerMapping& m) { return m.dest == r; });
            for (int c = 0; c < cols; ++c) {
                const MixerSource* srcPtr = nullptr;
                if (mapIt != stage.mixerMappings.end()) {
                    auto srcIt = std::find_if(mapIt->sources.begin(), mapIt->sources.end(),
                                              [c](const MixerSource& s) { return s.channel == c; });
                    if (srcIt != mapIt->sources.end())
                        srcPtr = &(*srcIt);
                }

                auto cellWidget = new QWidget(table);
                auto cellVBox = new QVBoxLayout(cellWidget);
                cellVBox->setContentsMargins(2, 2, 2, 2);
                cellVBox->setSpacing(2);

                auto checkBtn = new QCheckBox(cellWidget);
                checkBtn->setChecked(srcPtr != nullptr);
                cellVBox->addWidget(checkBtn, 0, Qt::AlignCenter);

                if (srcPtr) {
                    auto spin = new QDoubleSpinBox(cellWidget);
                    spin->setRange(-120.0, 30.0);
                    spin->setSingleStep(0.5);
                    spin->setValue(srcPtr->gainValue());
                    spin->setStyleSheet("font-size: 10px;");
                    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                            [this, &stage, r, c](double val) {
                                auto& mapList = stage.mixerMappings;
                                auto mapIt = std::find_if(mapList.begin(), mapList.end(),
                                                          [r](const MixerMapping& m) { return m.dest == r; });
                                if (mapIt != mapList.end()) {
                                    for (auto& s : mapIt->sources) {
                                        if (s.channel == c) {
                                            s.gain = val;
                                            break;
                                        }
                                    }
                                    applyConfig();
                                }
                            });
                    cellVBox->addWidget(spin);

                    auto btnHBox = new QHBoxLayout();
                    btnHBox->setSpacing(2);

                    auto invBtn = new QPushButton("Ø", cellWidget);
                    invBtn->setFixedSize(22, 20);
                    invBtn->setCheckable(true);
                    invBtn->setChecked(srcPtr->inverted.value_or(false));
                    invBtn->setStyleSheet(invBtn->isChecked() ? "background: orange; color: white;" : "color: gray;");
                    connect(invBtn, &QPushButton::clicked, [this, &stage, r, c, invBtn]() {
                        auto& mapList = stage.mixerMappings;
                        auto mapIt = std::find_if(mapList.begin(), mapList.end(),
                                                  [r](const MixerMapping& m) { return m.dest == r; });
                        if (mapIt != mapList.end()) {
                            for (auto& s : mapIt->sources) {
                                if (s.channel == c) {
                                    s.inverted = !s.inverted.value_or(false);
                                    break;
                                }
                            }
                            applyConfig();
                            refreshUi();
                        }
                    });
                    btnHBox->addWidget(invBtn);

                    auto muteBtn = new QPushButton("M", cellWidget);
                    muteBtn->setFixedSize(22, 20);
                    muteBtn->setCheckable(true);
                    muteBtn->setChecked(srcPtr->mute.value_or(false));
                    muteBtn->setStyleSheet(muteBtn->isChecked() ? "background: red; color: white;" : "color: gray;");
                    connect(muteBtn, &QPushButton::clicked, [this, &stage, r, c, muteBtn]() {
                        auto& mapList = stage.mixerMappings;
                        auto mapIt = std::find_if(mapList.begin(), mapList.end(),
                                                  [r](const MixerMapping& m) { return m.dest == r; });
                        if (mapIt != mapList.end()) {
                            for (auto& s : mapIt->sources) {
                                if (s.channel == c) {
                                    s.mute = !s.mute.value_or(false);
                                    break;
                                }
                            }
                            applyConfig();
                            refreshUi();
                        }
                    });
                    btnHBox->addWidget(muteBtn);

                    auto scaleBtn = new QPushButton(
                        (srcPtr->scale.value_or(GainScale::dB) == GainScale::dB) ? "dB" : "lin", cellWidget);
                    scaleBtn->setFixedSize(26, 20);
                    connect(scaleBtn, &QPushButton::clicked, [this, &stage, r, c, srcPtr]() {
                        auto& mapList = stage.mixerMappings;
                        auto mapIt = std::find_if(mapList.begin(), mapList.end(),
                                                  [r](const MixerMapping& m) { return m.dest == r; });
                        if (mapIt != mapList.end()) {
                            for (auto& s : mapIt->sources) {
                                if (s.channel == c) {
                                    s.scale = (s.scale.value_or(GainScale::dB) == GainScale::dB) ? GainScale::linear
                                                                                                 : GainScale::dB;
                                    break;
                                }
                            }
                            applyConfig();
                            refreshUi();
                        }
                    });
                    btnHBox->addWidget(scaleBtn);

                    cellVBox->addLayout(btnHBox);
                }

                connect(checkBtn, &QCheckBox::toggled, [this, &stage, r, c](bool checked) {
                    auto& mapList = stage.mixerMappings;
                    auto mapIt = std::find_if(mapList.begin(), mapList.end(),
                                              [r](const MixerMapping& m) { return m.dest == r; });
                    if (mapIt == mapList.end()) {
                        stage.mixerMappings.push_back(MixerMapping{r, {}, std::nullopt});
                        mapIt = std::prev(stage.mixerMappings.end());
                    }
                    if (checked) {
                        if (std::find_if(mapIt->sources.begin(), mapIt->sources.end(), [c](const MixerSource& s) {
                                return s.channel == c;
                            }) == mapIt->sources.end()) {
                            mapIt->sources.push_back(MixerSource{c, 0.0, false});
                        }
                    } else {
                        mapIt->sources.erase(std::remove_if(mapIt->sources.begin(), mapIt->sources.end(),
                                                            [c](const MixerSource& s) { return s.channel == c; }),
                                             mapIt->sources.end());
                    }
                    applyConfig();
                    refreshUi();
                });

                table->setCellWidget(r, c, cellWidget);
            }
        }
        matrixVBox->addWidget(table);

        auto resetBtn = new QPushButton("Reset to 1:1 Passthrough", matrixGroup);
        connect(resetBtn, &QPushButton::clicked, [this, &stage, rows, cols]() {
            int minCh = std::min(rows, cols);
            stage.mixerMappings.clear();
            for (int r = 0; r < rows; ++r) {
                int src = r < minCh ? r : 0;
                stage.mixerMappings.push_back(MixerMapping{r, {MixerSource{src, 0.0, false}}, std::nullopt});
            }
            applyConfig();
            refreshUi();
        });
        matrixVBox->addWidget(resetBtn);

        mmVBox->addWidget(matrixGroup);
        containerLayout->addWidget(mmGroup);
        break;
    }

    case StageType::Compressor: {
        auto compGroup = new QGroupBox("Dynamics Compressor", m_optionsContainer);
        auto formLayout = new QFormLayout(compGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto thSlider = new QSlider(Qt::Horizontal, compGroup);
        thSlider->setRange(-600, 0);
        thSlider->setValue(static_cast<int>(stage.compressorThreshold * 10.0));
        auto thLbl = new QLabel(QString("%1 dB").arg(stage.compressorThreshold, 0, 'f', 1), compGroup);
        thLbl->setFixedWidth(65);
        thLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        thLbl->setFont(QFont("monospace", 11));
        connect(thSlider, &QSlider::valueChanged, [this, &stage, thLbl](int val) {
            stage.compressorThreshold = val / 10.0;
            thLbl->setText(QString("%1 dB").arg(stage.compressorThreshold, 0, 'f', 1));
            applyConfig();
        });
        auto thBox = new QHBoxLayout();
        thBox->addWidget(thSlider);
        thBox->addWidget(thLbl);
        formLayout->addRow("Threshold:", thBox);

        auto ratioSlider = new QSlider(Qt::Horizontal, compGroup);
        ratioSlider->setRange(10, 200);
        ratioSlider->setValue(static_cast<int>(stage.compressorRatio * 10.0));
        auto ratioLbl = new QLabel(QString("%1:1").arg(stage.compressorRatio, 0, 'f', 1), compGroup);
        ratioLbl->setFixedWidth(65);
        ratioLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ratioLbl->setFont(QFont("monospace", 11));
        connect(ratioSlider, &QSlider::valueChanged, [this, &stage, ratioLbl](int val) {
            stage.compressorRatio = val / 10.0;
            ratioLbl->setText(QString("%1:1").arg(stage.compressorRatio, 0, 'f', 1));
            applyConfig();
        });
        auto ratioBox = new QHBoxLayout();
        ratioBox->addWidget(ratioSlider);
        ratioBox->addWidget(ratioLbl);
        formLayout->addRow("Ratio:", ratioBox);

        auto attSlider = new QSlider(Qt::Horizontal, compGroup);
        attSlider->setRange(1, 1000);
        attSlider->setValue(static_cast<int>(stage.compressorAttack * 10.0));
        auto attLbl = new QLabel(QString("%1 ms").arg(stage.compressorAttack, 0, 'f', 1), compGroup);
        attLbl->setFixedWidth(65);
        attLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        attLbl->setFont(QFont("monospace", 11));
        connect(attSlider, &QSlider::valueChanged, [this, &stage, attLbl](int val) {
            stage.compressorAttack = val / 10.0;
            attLbl->setText(QString("%1 ms").arg(stage.compressorAttack, 0, 'f', 1));
            applyConfig();
        });
        auto attBox = new QHBoxLayout();
        attBox->addWidget(attSlider);
        attBox->addWidget(attLbl);
        formLayout->addRow("Attack:", attBox);

        auto relSlider = new QSlider(Qt::Horizontal, compGroup);
        relSlider->setRange(5, 1000);
        relSlider->setValue(static_cast<int>(stage.compressorRelease));
        auto relLbl = new QLabel(QString("%1 ms").arg(static_cast<int>(stage.compressorRelease)), compGroup);
        relLbl->setFixedWidth(65);
        relLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        relLbl->setFont(QFont("monospace", 11));
        connect(relSlider, &QSlider::valueChanged, [this, &stage, relLbl](int val) {
            stage.compressorRelease = val;
            relLbl->setText(QString("%1 ms").arg(val));
            applyConfig();
        });
        auto relBox = new QHBoxLayout();
        relBox->addWidget(relSlider);
        relBox->addWidget(relLbl);
        formLayout->addRow("Release:", relBox);

        auto mkSlider = new QSlider(Qt::Horizontal, compGroup);
        mkSlider->setRange(0, 300);
        mkSlider->setValue(static_cast<int>(stage.compressorMakeupGain * 10.0));
        auto mkLbl = new QLabel(QString("%1%2 dB")
                                    .arg(stage.compressorMakeupGain >= 0 ? "+" : "")
                                    .arg(stage.compressorMakeupGain, 0, 'f', 1),
                                compGroup);
        mkLbl->setFixedWidth(65);
        mkLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mkLbl->setFont(QFont("monospace", 11));
        connect(mkSlider, &QSlider::valueChanged, [this, &stage, mkLbl](int val) {
            stage.compressorMakeupGain = val / 10.0;
            mkLbl->setText(QString("%1%2 dB")
                               .arg(stage.compressorMakeupGain >= 0 ? "+" : "")
                               .arg(stage.compressorMakeupGain, 0, 'f', 1));
            applyConfig();
        });
        auto mkBox = new QHBoxLayout();
        mkBox->addWidget(mkSlider);
        mkBox->addWidget(mkLbl);
        formLayout->addRow("Makeup Gain:", mkBox);

        auto monLayout = new QHBoxLayout();
        for (int c = 0; c < incomingChannels; ++c) {
            auto btn = new QPushButton(QString::number(c + 1), compGroup);
            btn->setFixedWidth(32);
            btn->setCheckable(true);
            bool isSelected =
                std::find(stage.monitorChannels.begin(), stage.monitorChannels.end(), c) != stage.monitorChannels.end();
            btn->setChecked(isSelected);
            btn->setStyleSheet(
                isSelected
                    ? "background: #007aff; color: white; border-radius: 4px; border: none;"
                    : "background: rgba(142, 142, 147, 0.15); color: palette(text); border-radius: 4px; border: none;");
            connect(btn, &QPushButton::clicked, [this, &stage, c, btn]() {
                auto it = std::find(stage.monitorChannels.begin(), stage.monitorChannels.end(), c);
                if (it != stage.monitorChannels.end()) {
                    if (stage.monitorChannels.size() > 1)
                        stage.monitorChannels.erase(it);
                } else {
                    stage.monitorChannels.push_back(c);
                }
                applyConfig();
                refreshUi();
            });
            monLayout->addWidget(btn);
        }
        monLayout->addStretch();
        formLayout->addRow("Monitor Ch:", monLayout);

        auto softChk = new QCheckBox("Enable Soft Clip", compGroup);
        softChk->setChecked(stage.compressorSoftClip);
        connect(softChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.compressorSoftClip = chk;
            applyConfig();
            refreshUi();
        });
        formLayout->addRow("", softChk);

        if (stage.compressorSoftClip) {
            auto clipSlider = new QSlider(Qt::Horizontal, compGroup);
            clipSlider->setRange(-100, 0);
            clipSlider->setValue(static_cast<int>(stage.compressorClipLimit * 10.0));
            auto clipLbl = new QLabel(QString("%1 dB").arg(stage.compressorClipLimit, 0, 'f', 1), compGroup);
            clipLbl->setFixedWidth(65);
            clipLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            clipLbl->setFont(QFont("monospace", 11));
            connect(clipSlider, &QSlider::valueChanged, [this, &stage, clipLbl](int val) {
                stage.compressorClipLimit = val / 10.0;
                clipLbl->setText(QString("%1 dB").arg(stage.compressorClipLimit, 0, 'f', 1));
                applyConfig();
            });
            auto clipBox = new QHBoxLayout();
            clipBox->addWidget(clipSlider);
            clipBox->addWidget(clipLbl);
            formLayout->addRow("Clip Limit:", clipBox);
        }

        containerLayout->addWidget(compGroup);
        break;
    }

    case StageType::NoiseGate: {
        auto gateGroup = new QGroupBox("Noise Gate", m_optionsContainer);
        auto formLayout = new QFormLayout(gateGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto thSlider = new QSlider(Qt::Horizontal, gateGroup);
        thSlider->setRange(-1000, 0);
        thSlider->setValue(static_cast<int>(stage.gateThreshold * 10.0));
        auto thLbl = new QLabel(QString("%1 dB").arg(stage.gateThreshold, 0, 'f', 1), gateGroup);
        thLbl->setFixedWidth(65);
        thLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        thLbl->setFont(QFont("monospace", 11));
        connect(thSlider, &QSlider::valueChanged, [this, &stage, thLbl](int val) {
            stage.gateThreshold = val / 10.0;
            thLbl->setText(QString("%1 dB").arg(stage.gateThreshold, 0, 'f', 1));
            applyConfig();
        });
        auto thBox = new QHBoxLayout();
        thBox->addWidget(thSlider);
        thBox->addWidget(thLbl);
        formLayout->addRow("Threshold:", thBox);

        auto attenSlider = new QSlider(Qt::Horizontal, gateGroup);
        attenSlider->setRange(-1000, 0);
        attenSlider->setValue(static_cast<int>(stage.gateAttenuation * 10.0));
        auto attenLbl = new QLabel(QString("%1 dB").arg(stage.gateAttenuation, 0, 'f', 1), gateGroup);
        attenLbl->setFixedWidth(65);
        attenLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        attenLbl->setFont(QFont("monospace", 11));
        connect(attenSlider, &QSlider::valueChanged, [this, &stage, attenLbl](int val) {
            stage.gateAttenuation = val / 10.0;
            attenLbl->setText(QString("%1 dB").arg(stage.gateAttenuation, 0, 'f', 1));
            applyConfig();
        });
        auto attenBox = new QHBoxLayout();
        attenBox->addWidget(attenSlider);
        attenBox->addWidget(attenLbl);
        formLayout->addRow("Attenuation:", attenBox);

        auto attSlider = new QSlider(Qt::Horizontal, gateGroup);
        attSlider->setRange(1, 1000);
        attSlider->setValue(static_cast<int>(stage.gateAttack * 10.0));
        auto attLbl = new QLabel(QString("%1 ms").arg(stage.gateAttack, 0, 'f', 1), gateGroup);
        attLbl->setFixedWidth(65);
        attLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        attLbl->setFont(QFont("monospace", 11));
        connect(attSlider, &QSlider::valueChanged, [this, &stage, attLbl](int val) {
            stage.gateAttack = val / 10.0;
            attLbl->setText(QString("%1 ms").arg(stage.gateAttack, 0, 'f', 1));
            applyConfig();
        });
        auto attBox = new QHBoxLayout();
        attBox->addWidget(attSlider);
        attBox->addWidget(attLbl);
        formLayout->addRow("Attack:", attBox);

        auto relSlider = new QSlider(Qt::Horizontal, gateGroup);
        relSlider->setRange(5, 1000);
        relSlider->setValue(static_cast<int>(stage.gateRelease));
        auto relLbl = new QLabel(QString("%1 ms").arg(static_cast<int>(stage.gateRelease)), gateGroup);
        connect(relSlider, &QSlider::valueChanged, [this, &stage, relLbl](int val) {
            stage.gateRelease = val;
            relLbl->setText(QString("%1 ms").arg(val));
            applyConfig();
        });
        auto relBox = new QHBoxLayout();
        relBox->addWidget(relSlider);
        relBox->addWidget(relLbl);
        formLayout->addRow("Release:", relBox);

        auto monLayout = new QHBoxLayout();
        for (int c = 0; c < incomingChannels; ++c) {
            auto btn = new QPushButton(QString::number(c + 1), gateGroup);
            btn->setFixedWidth(32);
            btn->setCheckable(true);
            bool isSelected =
                std::find(stage.monitorChannels.begin(), stage.monitorChannels.end(), c) != stage.monitorChannels.end();
            btn->setChecked(isSelected);
            btn->setStyleSheet(
                isSelected
                    ? "background: #007aff; color: white; border-radius: 4px; border: none;"
                    : "background: rgba(142, 142, 147, 0.15); color: palette(text); border-radius: 4px; border: none;");
            connect(btn, &QPushButton::clicked, [this, &stage, c, btn]() {
                auto it = std::find(stage.monitorChannels.begin(), stage.monitorChannels.end(), c);
                if (it != stage.monitorChannels.end()) {
                    if (stage.monitorChannels.size() > 1)
                        stage.monitorChannels.erase(it);
                } else {
                    stage.monitorChannels.push_back(c);
                }
                applyConfig();
                refreshUi();
            });
            monLayout->addWidget(btn);
        }
        monLayout->addStretch();
        formLayout->addRow("Monitor Ch:", monLayout);

        containerLayout->addWidget(gateGroup);
        break;
    }

    case StageType::RACE: {
        auto raceGroup = new QGroupBox("RACE Crosstalk Cancellation", m_optionsContainer);
        auto raceVBox = new QVBoxLayout(raceGroup);

        auto descLbl = new QLabel("Receiver Active Crosstalk Cancellation (RACE) implements a 3D audio effect for "
                                  "speaker playback by canceling acoustic crosstalk between two channels.",
                                  raceGroup);
        descLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        raceVBox->addWidget(descLbl);

        auto formLayout = new QFormLayout();
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto unitCombo = new QComboBox(raceGroup);
        unitCombo->addItems({"Milliseconds (ms)", "Microseconds (μs)", "Samples", "Millimeters (mm)"});
        unitCombo->setCurrentIndex(static_cast<int>(stage.raceDelayUnit));
        connect(unitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.raceDelayUnit = static_cast<DelayUnit>(idx);
            applyConfig();
            refreshUi();
        });
        formLayout->addRow("Delay Unit:", unitCombo);

        double minVal =
            (stage.raceDelayUnit == DelayUnit::samples)
                ? 1.0
                : ((stage.raceDelayUnit == DelayUnit::us) ? 5.0
                                                          : ((stage.raceDelayUnit == DelayUnit::mm) ? 2.0 : 0.01));
        double maxVal =
            (stage.raceDelayUnit == DelayUnit::samples)
                ? 100.0
                : ((stage.raceDelayUnit == DelayUnit::us) ? 2000.0
                                                          : ((stage.raceDelayUnit == DelayUnit::mm) ? 700.0 : 2.0));
        double stepVal = (stage.raceDelayUnit == DelayUnit::samples)
                             ? (stage.raceSubsampleDelay ? 0.01 : 1.0)
                             : ((stage.raceDelayUnit == DelayUnit::ms) ? 0.01 : 1.0);

        int stepsCount = static_cast<int>((maxVal - minVal) / stepVal);
        auto delaySlider = new QSlider(Qt::Horizontal, raceGroup);
        delaySlider->setRange(0, stepsCount);
        delaySlider->setValue(static_cast<int>((stage.raceDelay - minVal) / stepVal));

        auto delayLbl = new QLabel(QString("%1 %2")
                                       .arg(stage.raceDelay, 0, 'f', 2)
                                       .arg(QString::fromStdString(delayUnitToString(stage.raceDelayUnit))),
                                   raceGroup);
        delayLbl->setFixedWidth(80);
        delayLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        delayLbl->setFont(QFont("monospace", 11));
        connect(delaySlider, &QSlider::valueChanged, [this, &stage, delayLbl, minVal, stepVal](int val) {
            stage.raceDelay = minVal + val * stepVal;
            delayLbl->setText(QString("%1 %2")
                                  .arg(stage.raceDelay, 0, 'f', 2)
                                  .arg(QString::fromStdString(delayUnitToString(stage.raceDelayUnit))));
            applyConfig();
        });
        auto delayBox = new QHBoxLayout();
        delayBox->addWidget(delaySlider);
        delayBox->addWidget(delayLbl);
        formLayout->addRow("Delay:", delayBox);

        auto attenSlider = new QSlider(Qt::Horizontal, raceGroup);
        attenSlider->setRange(10, 200);
        attenSlider->setValue(static_cast<int>(stage.raceAttenuation * 10.0));
        auto attenLbl = new QLabel(QString("%1 dB").arg(stage.raceAttenuation, 0, 'f', 1), raceGroup);
        attenLbl->setFixedWidth(65);
        attenLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        attenLbl->setFont(QFont("monospace", 11));
        connect(attenSlider, &QSlider::valueChanged, [this, &stage, attenLbl](int val) {
            stage.raceAttenuation = val / 10.0;
            attenLbl->setText(QString("%1 dB").arg(stage.raceAttenuation, 0, 'f', 1));
            applyConfig();
        });
        auto attenBox = new QHBoxLayout();
        attenBox->addWidget(attenSlider);
        attenBox->addWidget(attenLbl);
        formLayout->addRow("Attenuation:", attenBox);

        auto subChk = new QCheckBox("Subsample Delay (uses IIR allpass filter)", raceGroup);
        subChk->setChecked(stage.raceSubsampleDelay);
        connect(subChk, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.raceSubsampleDelay = chk;
            applyConfig();
        });
        formLayout->addRow("", subChk);

        raceVBox->addLayout(formLayout);
        containerLayout->addWidget(raceGroup);
        break;
    }

    case StageType::Dither: {
        auto ditherGroup = new QGroupBox("Dither Noise Shaping", m_optionsContainer);
        auto formLayout = new QFormLayout(ditherGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

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
                                                      DitherType::Shibata96,
                                                      DitherType::ShibataLow96};
        for (auto t : types) {
            typeCombo->addItem(QString::fromStdString(ditherTypeToString(t)), static_cast<int>(t));
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
        formLayout->addRow("Type:", typeCombo);

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
        formLayout->addRow("Bit Depth:", bitsCombo);

        auto ampSlider = new QSlider(Qt::Horizontal, ditherGroup);
        ampSlider->setRange(0, 1000);
        ampSlider->setValue(static_cast<int>(stage.ditherAmplitude * 10.0));
        auto ampLbl = new QLabel(QString("%1").arg(stage.ditherAmplitude, 0, 'f', 1), ditherGroup);
        ampLbl->setFixedWidth(65);
        ampLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ampLbl->setFont(QFont("monospace", 11));
        connect(ampSlider, &QSlider::valueChanged, [this, &stage, ampLbl](int val) {
            stage.ditherAmplitude = val / 10.0;
            ampLbl->setText(QString("%1").arg(stage.ditherAmplitude, 0, 'f', 1));
            applyConfig();
        });
        auto ampBox = new QHBoxLayout();
        ampBox->addWidget(ampSlider);
        ampBox->addWidget(ampLbl);
        formLayout->addRow("Amplitude:", ampBox);

        containerLayout->addWidget(ditherGroup);
        break;
    }

    case StageType::DiffEq: {
        auto deGroup = new QGroupBox("Differential Equation Filter", m_optionsContainer);
        auto deVBox = new QVBoxLayout(deGroup);

        auto descLbl = new QLabel(
            "Direct form II IIR filter coefficients. Specify as comma-separated lists of decimal numbers.", deGroup);
        descLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        deVBox->addWidget(descLbl);

        auto formLayout = new QFormLayout();
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto bEdit = new QLineEdit(QString::fromStdString(stage.diffEqB), deGroup);
        bEdit->setPlaceholderText("e.g. 1.0, 0.5, 0.25");
        connect(bEdit, &QLineEdit::editingFinished, [this, &stage, bEdit]() {
            stage.diffEqB = bEdit->text().toStdString();
            applyConfig();
        });
        formLayout->addRow("Feedforward Coeffs (b):", bEdit);

        auto aEdit = new QLineEdit(QString::fromStdString(stage.diffEqA), deGroup);
        aEdit->setPlaceholderText("e.g. 1.0, -0.5, 0.1");
        connect(aEdit, &QLineEdit::editingFinished, [this, &stage, aEdit]() {
            stage.diffEqA = aEdit->text().toStdString();
            applyConfig();
        });
        formLayout->addRow("Feedback Coeffs (a):", aEdit);

        deVBox->addLayout(formLayout);
        containerLayout->addWidget(deGroup);
        break;
    }

    case StageType::BiquadCombo: {
        auto comboGroup = new QGroupBox("Biquad Combo / Crossovers", m_optionsContainer);
        auto formLayout = new QFormLayout(comboGroup);
        formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto typeCombo = new QComboBox(comboGroup);
        typeCombo->addItems({"Butterworth Lowpass", "Butterworth Highpass", "Linkwitz-Riley Lowpass",
                             "Linkwitz-Riley Highpass", "Tilt", "Five-Point PEQ"});
        typeCombo->setCurrentIndex(static_cast<int>(stage.comboType));
        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.comboType = static_cast<BiquadComboType>(idx);
            applyConfig();
            refreshUi();
        });
        formLayout->addRow("Combo Type:", typeCombo);

        if (stage.comboType != BiquadComboType::FivePointPeq) {
            auto freqSlider = new QSlider(Qt::Horizontal, comboGroup);
            freqSlider->setRange(20, 20000);
            freqSlider->setValue(static_cast<int>(stage.comboFreq));
            auto freqLbl = new QLabel(QString("%1 Hz").arg(static_cast<int>(stage.comboFreq)), comboGroup);
            freqLbl->setFixedWidth(65);
            freqLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            freqLbl->setFont(QFont("monospace", 11));
            connect(freqSlider, &QSlider::valueChanged, [this, &stage, freqLbl](int val) {
                stage.comboFreq = val;
                freqLbl->setText(QString("%1 Hz").arg(val));
                applyConfig();
            });
            auto freqBox = new QHBoxLayout();
            freqBox->addWidget(freqSlider);
            freqBox->addWidget(freqLbl);
            formLayout->addRow("Frequency:", freqBox);
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
            connect(orderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    [this, &stage, orderCombo](int index) {
                        stage.comboOrder = orderCombo->itemData(index).toInt();
                        applyConfig();
                    });
            formLayout->addRow("Filter Order:", orderCombo);
        }

        if (stage.comboType == BiquadComboType::Tilt) {
            auto gainSlider = new QSlider(Qt::Horizontal, comboGroup);
            gainSlider->setRange(-999, 999);
            gainSlider->setValue(static_cast<int>(stage.comboGain * 10.0));
            auto gainLbl = new QLabel(
                QString("%1%2 dB").arg(stage.comboGain >= 0 ? "+" : "").arg(stage.comboGain, 0, 'f', 1), comboGroup);
            gainLbl->setFixedWidth(65);
            gainLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            gainLbl->setFont(QFont("monospace", 11));
            connect(gainSlider, &QSlider::valueChanged, [this, &stage, gainLbl](int val) {
                stage.comboGain = val / 10.0;
                gainLbl->setText(
                    QString("%1%2 dB").arg(stage.comboGain >= 0 ? "+" : "").arg(stage.comboGain, 0, 'f', 1));
                applyConfig();
            });
            auto gainBox = new QHBoxLayout();
            gainBox->addWidget(gainSlider);
            gainBox->addWidget(gainLbl);
            formLayout->addRow("Gain:", gainBox);
        }

        if (stage.comboType == BiquadComboType::FivePointPeq) {
            auto peqGroup = new QGroupBox("5-Point Parametric EQ", comboGroup);
            auto peqGrid = new QGridLayout(peqGroup);
            peqGrid->addWidget(new QLabel("Band", peqGroup), 0, 0);
            peqGrid->addWidget(new QLabel("Frequency (Hz)", peqGroup), 0, 1);
            peqGrid->addWidget(new QLabel("Gain (dB)", peqGroup), 0, 2);
            peqGrid->addWidget(new QLabel("Q", peqGroup), 0, 3);

            auto addRow = [this, peqGroup, peqGrid](int r, const QString& name, double* f, double* g, double* q) {
                peqGrid->addWidget(new QLabel(name, peqGroup), r, 0);

                auto fSpin = new QDoubleSpinBox(peqGroup);
                fSpin->setRange(20.0, 20000.0);
                fSpin->setSingleStep(10.0);
                fSpin->setDecimals(1);
                fSpin->setValue(*f);
                fSpin->setFixedWidth(90);
                connect(fSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, f](double val) {
                    *f = val;
                    applyConfig();
                });
                peqGrid->addWidget(fSpin, r, 1);

                auto gSpin = new QDoubleSpinBox(peqGroup);
                gSpin->setRange(-40.0, 40.0);
                gSpin->setSingleStep(0.5);
                gSpin->setDecimals(1);
                gSpin->setValue(*g);
                gSpin->setFixedWidth(80);
                connect(gSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, g](double val) {
                    *g = val;
                    applyConfig();
                });
                peqGrid->addWidget(gSpin, r, 2);

                auto qSpin = new QDoubleSpinBox(peqGroup);
                qSpin->setRange(0.05, 100.0);
                qSpin->setSingleStep(0.05);
                qSpin->setDecimals(3);
                qSpin->setValue(*q);
                qSpin->setFixedWidth(80);
                connect(qSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, q](double val) {
                    *q = val;
                    applyConfig();
                });
                peqGrid->addWidget(qSpin, r, 3);
            };

            addRow(1, "Low Shelf", &stage.peqFls, &stage.peqGls, &stage.peqQls);
            addRow(2, "PEQ 1", &stage.peqF1, &stage.peqG1, &stage.peqQ1);
            addRow(3, "PEQ 2", &stage.peqF2, &stage.peqG2, &stage.peqQ2);
            addRow(4, "PEQ 3", &stage.peqF3, &stage.peqG3, &stage.peqQ3);
            addRow(5, "High Shelf", &stage.peqFhs, &stage.peqGhs, &stage.peqQhs);

            formLayout->addRow(peqGroup);
        }

        containerLayout->addWidget(comboGroup);
        break;
    }

    case StageType::GraphicEQ: {
        auto geqGroup = new QGroupBox("Graphic Equalizer Settings", m_optionsContainer);
        auto geqVBox = new QVBoxLayout(geqGroup);

        auto topHBox = new QHBoxLayout();

        auto rangeBox = new QVBoxLayout();
        rangeBox->addWidget(new QLabel("Frequency Range", geqGroup));
        auto rangeHBox = new QHBoxLayout();
        auto minEdit = new QLineEdit(QString::number(stage.graphicEQFreqMin), geqGroup);
        minEdit->setFixedWidth(60);
        connect(minEdit, &QLineEdit::editingFinished, [this, &stage, minEdit]() {
            stage.graphicEQFreqMin = minEdit->text().toDouble();
            applyConfig();
            refreshUi();
        });
        rangeHBox->addWidget(minEdit);
        rangeHBox->addWidget(new QLabel("to", geqGroup));
        auto maxEdit = new QLineEdit(QString::number(stage.graphicEQFreqMax), geqGroup);
        maxEdit->setFixedWidth(70);
        connect(maxEdit, &QLineEdit::editingFinished, [this, &stage, maxEdit]() {
            stage.graphicEQFreqMax = maxEdit->text().toDouble();
            applyConfig();
            refreshUi();
        });
        rangeHBox->addWidget(maxEdit);
        rangeHBox->addWidget(new QLabel("Hz", geqGroup));
        rangeBox->addLayout(rangeHBox);
        topHBox->addLayout(rangeBox);

        auto bandsBox = new QVBoxLayout();
        bandsBox->addWidget(new QLabel("Bands", geqGroup));
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
        bandsBox->addWidget(spinBands);
        topHBox->addLayout(bandsBox);

        topHBox->addStretch();
        geqVBox->addLayout(topHBox);

        // Scrollable Slider Bank for 31-band ISO frequencies
        auto scrollBank = new QScrollArea(geqGroup);
        scrollBank->setWidgetResizable(true);
        scrollBank->setFixedHeight(220);

        auto bankContainer = new QWidget(scrollBank);
        auto bankLayout = new QHBoxLayout(bankContainer);
        bankLayout->setSpacing(8);
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

        static const char* const iso31Labels[31] = {"20",  "25",   "31.5",  "40",   "50",    "63",   "80",    "100",
                                                    "125", "160",  "200",   "250",  "315",   "400",  "500",   "630",
                                                    "800", "1k",   "1.25k", "1.6k", "2k",    "2.5k", "3.15k", "4k",
                                                    "5k",  "6.3k", "8k",    "10k",  "12.5k", "16k",  "20k"};

        for (int b = 0; b < totalBands; ++b) {
            QString fText;
            if (totalBands == 31 && std::abs(stage.graphicEQFreqMin - 20.0) < 1e-3 &&
                std::abs(stage.graphicEQFreqMax - 20000.0) < 1e-3) {
                fText = QString(iso31Labels[b]);
            } else {
                double freq = bandFrequency(b, totalBands, stage.graphicEQFreqMin, stage.graphicEQFreqMax);
                fText = freqLabelText(freq);
            }

            auto bVBox = new QVBoxLayout();
            bVBox->setSpacing(4);

            auto gainValLbl = new QLabel(QString("%1").arg(stage.graphicEQGains[b], 0, 'f', 1), bankContainer);
            gainValLbl->setFont(QFont("monospace", 8));
            gainValLbl->setAlignment(Qt::AlignCenter);
            bVBox->addWidget(gainValLbl);

            auto slider = new VSliderWidget(stage.graphicEQGains[b], -40.0, 40.0, bankContainer);
            connect(slider, &VSliderWidget::valueChanged, [this, &stage, b, gainValLbl](double val) {
                stage.graphicEQGains[b] = val;
                gainValLbl->setText(QString("%1").arg(val, 0, 'f', 1));
                applyConfig();
            });
            bVBox->addWidget(slider, 0, Qt::AlignCenter);

            auto fLbl = new QLabel(fText, bankContainer);
            fLbl->setFont(QFont("sans-serif", 8, QFont::Bold));
            fLbl->setAlignment(Qt::AlignCenter);
            bVBox->addWidget(fLbl);

            bankLayout->addLayout(bVBox);
        }

        scrollBank->setWidget(bankContainer);
        geqVBox->addWidget(scrollBank);

        auto resetGainsBtn = new QPushButton("Reset All Bands to 0 dB", geqGroup);
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
