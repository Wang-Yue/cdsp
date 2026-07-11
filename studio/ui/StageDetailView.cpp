#include "ui/StageDetailView.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QPushButton>
#include <QHeaderView>
#include <cmath>

StageDetailView::StageDetailView(
    size_t stageIndex,
    std::shared_ptr<PipelineStore> pipeline,
    std::shared_ptr<DSPEngineController> dspController,
    QWidget* parent
) : QWidget(parent), m_stageIndex(stageIndex), m_pipeline(pipeline), m_dspController(dspController) {
    setupUi();
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
    m_pipeline->save();
    emit m_pipeline->pipelineChanged();
    m_dspController->applyConfig();
}

void StageDetailView::refreshUi() {
    if (m_stageIndex >= m_pipeline->stages.size()) return;
    const auto& stage = m_pipeline->stages[m_stageIndex];

    m_nameEdit->setText(QString::fromStdString(stage.name));
    m_enabledCheck->setChecked(stage.isEnabled);

    QMetaObject::invokeMethod(this, [this]() {
        buildStageOptionsUi();
    }, Qt::QueuedConnection);
}

void StageDetailView::buildStageOptionsUi() {
    auto containerLayout = qobject_cast<QVBoxLayout*>(m_optionsContainer->layout());
    QLayoutItem* item;
    while ((item = containerLayout->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (m_stageIndex >= m_pipeline->stages.size()) return;
    auto& stage = m_pipeline->stages[m_stageIndex];

    int channelCount = 8; // Available system processing channels

    // 1. Channel Selector Card
    if (stage.type != StageType::MatrixMixer) {
        auto chanGroup = new QGroupBox("Target Channels", m_optionsContainer);
        auto chanLayout = new QVBoxLayout(chanGroup);

        if (stage.type == StageType::Balance || stage.type == StageType::Width || stage.type == StageType::MSProc ||
            stage.type == StageType::Crossfeed || stage.type == StageType::RACE || stage.type == StageType::SplitWidth) {
            auto pairBox = new QHBoxLayout();

            auto leftBox = new QVBoxLayout();
            leftBox->addWidget(new QLabel("Left Input:", chanGroup));
            auto leftCombo = new QComboBox(chanGroup);
            for (int c = 0; c < channelCount; ++c) {
                leftCombo->addItem(QString("Channel %1").arg(c + 1), c);
            }
            leftCombo->setCurrentIndex(std::min(stage.leftChannel, channelCount - 1));
            connect(leftCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, leftCombo](int idx) {
                stage.leftChannel = leftCombo->itemData(idx).toInt();
                applyConfig();
            });
            leftBox->addWidget(leftCombo);
            pairBox->addLayout(leftBox);

            auto rightBox = new QVBoxLayout();
            rightBox->addWidget(new QLabel("Right Input:", chanGroup));
            auto rightCombo = new QComboBox(chanGroup);
            for (int c = 0; c < channelCount; ++c) {
                rightCombo->addItem(QString("Channel %1").arg(c + 1), c);
            }
            rightCombo->setCurrentIndex(std::min(stage.rightChannel, channelCount - 1));
            connect(rightCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, rightCombo](int idx) {
                stage.rightChannel = rightCombo->itemData(idx).toInt();
                applyConfig();
            });
            rightBox->addWidget(rightCombo);
            pairBox->addLayout(rightBox);

            pairBox->addStretch();
            chanLayout->addLayout(pairBox);

            auto descLbl = new QLabel("This stereo stage will process the selected Left and Right channels. All other channels will pass through unaffected.", chanGroup);
            descLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
            chanLayout->addWidget(descLbl);
        } else {
            auto pillsLayout = new QHBoxLayout();
            for (int c = 0; c < channelCount; ++c) {
                auto btn = new QPushButton(QString::number(c + 1), chanGroup);
                btn->setFixedWidth(36);
                btn->setCheckable(true);
                bool isSelected = std::find(stage.channels.begin(), stage.channels.end(), c) != stage.channels.end();
                btn->setChecked(isSelected);

                auto updateBtnStyle = [btn](bool checked) {
                    if (checked) {
                        btn->setStyleSheet("background-color: #007aff; color: white; font-weight: bold; border-radius: 4px;");
                    } else {
                        btn->setStyleSheet("background-color: #e5e5ea; color: #000000; border-radius: 4px;");
                    }
                };
                updateBtnStyle(isSelected);

                connect(btn, &QPushButton::clicked, [this, &stage, c, btn, updateBtnStyle]() {
                    auto it = std::find(stage.channels.begin(), stage.channels.end(), c);
                    if (it != stage.channels.end()) {
                        if (stage.channels.size() > 1) {
                            stage.channels.erase(it);
                            btn->setChecked(false);
                        } else {
                            btn->setChecked(true);
                        }
                    } else {
                        stage.channels.push_back(c);
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

    auto optsGroup = new QGroupBox(QString::fromStdString(stageTypeToString(stage.type)) + " Options", m_optionsContainer);
    auto optsForm = new QFormLayout(optsGroup);

    switch (stage.type) {
    case StageType::Balance: {
        auto balBox = new QHBoxLayout();
        auto balSlider = new QSlider(Qt::Horizontal, optsGroup);
        balSlider->setRange(-100, 100);
        balSlider->setValue(static_cast<int>(stage.balanceOffset * 100.0));
        balBox->addWidget(balSlider);

        auto balLbl = new QLabel(QString("%1%").arg(static_cast<int>(stage.balanceOffset * 100.0)), optsGroup);
        balLbl->setFixedWidth(60);
        balBox->addWidget(balLbl);

        auto centerBtn = new QPushButton("Center", optsGroup);
        connect(centerBtn, &QPushButton::clicked, [this, &stage, balSlider, balLbl]() {
            stage.balanceOffset = 0.0;
            balSlider->setValue(0);
            balLbl->setText("0%");
            applyConfig();
        });
        balBox->addWidget(centerBtn);

        connect(balSlider, &QSlider::valueChanged, [this, &stage, balLbl](int val) {
            stage.balanceOffset = val / 100.0;
            balLbl->setText(QString("%1%").arg(val));
            applyConfig();
        });
        optsForm->addRow("Balance Position:", balBox);
        break;
    }

    case StageType::Width: {
        auto widthBox = new QHBoxLayout();
        auto widthSlider = new QSlider(Qt::Horizontal, optsGroup);
        widthSlider->setRange(-100, 200);
        widthSlider->setValue(static_cast<int>(stage.widthFactor * 100.0));
        widthBox->addWidget(widthSlider);

        auto widthLbl = new QLabel(QString("%1%").arg(static_cast<int>(stage.widthFactor * 100.0)), optsGroup);
        widthLbl->setFixedWidth(60);
        widthBox->addWidget(widthLbl);

        auto monoBtn = new QPushButton("Mono", optsGroup);
        connect(monoBtn, &QPushButton::clicked, [this, &stage, widthSlider, widthLbl]() {
            stage.widthFactor = 0.0;
            widthSlider->setValue(0);
            widthLbl->setText("0%");
            applyConfig();
        });
        widthBox->addWidget(monoBtn);

        connect(widthSlider, &QSlider::valueChanged, [this, &stage, widthLbl](int val) {
            stage.widthFactor = val / 100.0;
            widthLbl->setText(QString("%1%").arg(val));
            applyConfig();
        });
        optsForm->addRow("Stereo Width Factor:", widthBox);
        break;
    }

    case StageType::MSProc: {
        optsForm->addRow(new QLabel("Converts Mid/Side signals into L/R stereo or vice-versa.", optsGroup));
        break;
    }

    case StageType::PhaseInvert: {
        auto invL = new QCheckBox("Invert Left Channel", optsGroup);
        invL->setChecked(stage.invertLeft);
        connect(invL, &QCheckBox::toggled, [this, &stage](bool chk) { stage.invertLeft = chk; applyConfig(); });
        optsForm->addRow("", invL);

        auto invR = new QCheckBox("Invert Right Channel", optsGroup);
        invR->setChecked(stage.invertRight);
        connect(invR, &QCheckBox::toggled, [this, &stage](bool chk) { stage.invertRight = chk; applyConfig(); });
        optsForm->addRow("", invR);
        break;
    }

    case StageType::Crossfeed: {
        auto levelCombo = new QComboBox(optsGroup);
        levelCombo->addItems({"Bauer", "Meier", "Moy", "Custom"});
        levelCombo->setCurrentIndex(static_cast<int>(stage.crossfeedPreset));
        connect(levelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.crossfeedPreset = static_cast<CrossfeedPreset>(idx);
            applyConfig();
            refreshUi();
        });
        optsForm->addRow("Preset Level:", levelCombo);

        if (stage.crossfeedPreset == CrossfeedPreset::Custom) {
            auto fcSpin = new QDoubleSpinBox(optsGroup);
            fcSpin->setRange(300, 1200); fcSpin->setValue(stage.crossfeedCutoff); fcSpin->setSuffix(" Hz");
            connect(fcSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
                stage.crossfeedCutoff = val; applyConfig();
            });
            optsForm->addRow("Custom Fc:", fcSpin);

            auto dbSpin = new QDoubleSpinBox(optsGroup);
            dbSpin->setRange(-30, 0); dbSpin->setValue(stage.crossfeedFeedDB); dbSpin->setSuffix(" dB");
            connect(dbSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
                stage.crossfeedFeedDB = val; applyConfig();
            });
            optsForm->addRow("Custom Feed DB:", dbSpin);
        }

        // Live computed parameter derivation status
        QString paramSummary = QString("Lowshelf Cutoff: %1 Hz | Lowpass Cutoff: %2 Hz | Feed: %3 dB (Q=0.5)")
            .arg(stage.crossfeedCutoff)
            .arg(stage.crossfeedCutoff)
            .arg(stage.crossfeedFeedDB, 0, 'f', 1);
        auto previewLbl = new QLabel(paramSummary, optsGroup);
        previewLbl->setStyleSheet("color: #34c759; font-weight: bold; font-size: 11px;");
        optsForm->addRow("Derived Filters:", previewLbl);
        break;
    }

    case StageType::SplitWidth: {
        auto freqSpin = new QDoubleSpinBox(optsGroup);
        freqSpin->setRange(100.0, 10000.0); freqSpin->setValue(stage.splitFreq); freqSpin->setSuffix(" Hz");
        connect(freqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.splitFreq = val; applyConfig();
        });
        optsForm->addRow("Crossover Frequency:", freqSpin);

        auto lowSpin = new QDoubleSpinBox(optsGroup);
        lowSpin->setRange(0.0, 2.0); lowSpin->setValue(stage.lowWidth);
        connect(lowSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.lowWidth = val; applyConfig();
        });
        optsForm->addRow("Bass Width:", lowSpin);

        auto highSpin = new QDoubleSpinBox(optsGroup);
        highSpin->setRange(0.0, 2.0); highSpin->setValue(stage.highWidth);
        connect(highSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.highWidth = val; applyConfig();
        });
        optsForm->addRow("Treble Width:", highSpin);
        break;
    }

    case StageType::GraphicEQ: {
        if (stage.graphicEqGains.size() != 10) {
            stage.graphicEqGains.assign(10, 0.0);
        }
        static const char* bandLabels[] = {"31Hz", "63Hz", "125Hz", "250Hz", "500Hz", "1kHz", "2kHz", "4kHz", "8kHz", "16kHz"};

        auto faderBankLayout = new QHBoxLayout();
        for (int b = 0; b < 10; ++b) {
            auto bLayout = new QVBoxLayout();
            auto valLbl = new QLabel(QString("%1dB").arg(stage.graphicEqGains[b], 0, 'f', 1), optsGroup);
            valLbl->setAlignment(Qt::AlignCenter);
            valLbl->setFont(QFont("monospace", 8));

            auto slider = new QSlider(Qt::Vertical, optsGroup);
            slider->setRange(-24, 24);
            slider->setValue(static_cast<int>(stage.graphicEqGains[b] * 2.0));
            slider->setFixedHeight(120);

            connect(slider, &QSlider::valueChanged, [this, &stage, b, valLbl](int val) {
                double db = val / 2.0;
                stage.graphicEqGains[b] = db;
                valLbl->setText(QString("%1dB").arg(db, 0, 'f', 1));
                applyConfig();
            });

            auto nameLbl = new QLabel(bandLabels[b], optsGroup);
            nameLbl->setAlignment(Qt::AlignCenter);
            nameLbl->setFont(QFont("sans-serif", 9, QFont::Bold));

            bLayout->addWidget(valLbl);
            bLayout->addWidget(slider, 0, Qt::AlignCenter);
            bLayout->addWidget(nameLbl);
            faderBankLayout->addLayout(bLayout);
        }
        optsForm->addRow("Band Gains:", faderBankLayout);

        auto resetGainsBtn = new QPushButton("Reset All Bands to 0 dB", optsGroup);
        connect(resetGainsBtn, &QPushButton::clicked, [this, &stage]() {
            stage.graphicEqGains.assign(10, 0.0);
            applyConfig();
            refreshUi();
        });
        optsForm->addRow("", resetGainsBtn);
        break;
    }

    case StageType::Loudness: {
        auto faderCombo = new QComboBox(optsGroup);
        faderCombo->addItems({"Main", "Aux 1", "Aux 2", "Aux 3", "Aux 4"});
        faderCombo->setCurrentIndex(static_cast<int>(stage.loudnessFader));
        connect(faderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.loudnessFader = static_cast<Fader>(idx); applyConfig();
        });
        optsForm->addRow("Target Fader:", faderCombo);

        auto refSpin = new QDoubleSpinBox(optsGroup);
        refSpin->setRange(40.0, 100.0); refSpin->setValue(stage.loudnessRefLevel); refSpin->setSuffix(" dB SPL");
        connect(refSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.loudnessRefLevel = val; applyConfig();
        });
        optsForm->addRow("Reference Level:", refSpin);

        auto lowSpin = new QDoubleSpinBox(optsGroup);
        lowSpin->setRange(0.0, 20.0); lowSpin->setValue(stage.loudnessLowBoost); lowSpin->setSuffix(" dB");
        connect(lowSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.loudnessLowBoost = val; applyConfig();
        });
        optsForm->addRow("Low Frequency Boost:", lowSpin);

        auto highSpin = new QDoubleSpinBox(optsGroup);
        highSpin->setRange(0.0, 20.0); highSpin->setValue(stage.loudnessHighBoost); highSpin->setSuffix(" dB");
        connect(highSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.loudnessHighBoost = val; applyConfig();
        });
        optsForm->addRow("High Frequency Boost:", highSpin);

        auto attChk = new QCheckBox("Attenuate Midrange", optsGroup);
        attChk->setChecked(stage.loudnessAttenuateMid);
        connect(attChk, &QCheckBox::toggled, [this, &stage](bool chk) { stage.loudnessAttenuateMid = chk; applyConfig(); });
        optsForm->addRow("", attChk);
        break;
    }

    case StageType::Emphasis: {
        auto modeCombo = new QComboBox(optsGroup);
        modeCombo->addItems({"De-Emphasis (50μs/15μs)", "Pre-Emphasis"});
        modeCombo->setCurrentIndex(stage.deEmphasis ? 0 : 1);
        connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.deEmphasis = (idx == 0); applyConfig();
        });
        optsForm->addRow("Emphasis Mode:", modeCombo);

        auto descLbl = new QLabel("Applies standard 50μs/15μs pre/de-emphasis digital filter curve.", optsGroup);
        descLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        optsForm->addRow("", descLbl);
        break;
    }

    case StageType::DCProtection: {
        auto descLbl = new QLabel("First-order highpass filter — removes DC offset and subsonic rumble on all channels.", optsGroup);
        descLbl->setStyleSheet("color: #8e8e93; font-size: 11px;");
        optsForm->addRow("", descLbl);

        auto freqSpin = new QDoubleSpinBox(optsGroup);
        freqSpin->setRange(1.0, 50.0); freqSpin->setValue(stage.dcCutoffFreq); freqSpin->setSuffix(" Hz");
        connect(freqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.dcCutoffFreq = val; applyConfig();
        });
        optsForm->addRow("Highpass Cutoff:", freqSpin);
        break;
    }

    case StageType::Gain: {
        auto gainBox = new QHBoxLayout();
        auto gainSlider = new QDoubleSpinBox(optsGroup);
        gainSlider->setRange(-150.0, 150.0); gainSlider->setValue(stage.gainDB); gainSlider->setSuffix(" dB");
        connect(gainSlider, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.gainDB = val; applyConfig();
        });
        gainBox->addWidget(gainSlider);

        auto invCheck = new QCheckBox("Invert Polarity", optsGroup);
        invCheck->setChecked(stage.gainInverted);
        connect(invCheck, &QCheckBox::toggled, [this, &stage](bool checked) {
            stage.gainInverted = checked; applyConfig();
        });
        gainBox->addWidget(invCheck);

        auto muteCheck = new QCheckBox("Mute", optsGroup);
        muteCheck->setChecked(stage.gainMuted);
        connect(muteCheck, &QCheckBox::toggled, [this, &stage](bool checked) {
            stage.gainMuted = checked; applyConfig();
        });
        gainBox->addWidget(muteCheck);

        optsForm->addRow("Gain Settings:", gainBox);
        break;
    }

    case StageType::Delay: {
        auto valSpin = new QDoubleSpinBox(optsGroup);
        valSpin->setRange(0.0, 10000.0); valSpin->setValue(stage.delayValue);
        connect(valSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.delayValue = val; applyConfig();
        });
        optsForm->addRow("Delay Value:", valSpin);

        auto unitCombo = new QComboBox(optsGroup);
        unitCombo->addItems({"ms", "us", "samples", "mm"});
        unitCombo->setCurrentText(QString::fromStdString(delayUnitToString(stage.delayUnit)));
        connect(unitCombo, &QComboBox::currentTextChanged, [this, &stage](const QString& text) {
            stage.delayUnit = stringToDelayUnit(text.toStdString()); applyConfig();
        });
        optsForm->addRow("Delay Unit:", unitCombo);

        auto subCheck = new QCheckBox("Enable Sub-sample Delay", optsGroup);
        subCheck->setChecked(stage.delaySubsample);
        connect(subCheck, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.delaySubsample = chk; applyConfig();
        });
        optsForm->addRow("", subCheck);
        break;
    }

    case StageType::EQ: {
        auto eqCombo = new QComboBox(optsGroup);
        eqCombo->addItem("None", QString());
        for (const auto& preset : m_pipeline->eqPresets) {
            eqCombo->addItem(QString::fromStdString(preset.name), preset.id.toString());
        }
        if (stage.eqPresetId.has_value()) {
            int idx = eqCombo->findData(stage.eqPresetId->toString());
            if (idx >= 0) eqCombo->setCurrentIndex(idx);
        }
        connect(eqCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, eqCombo]() {
            QString idStr = eqCombo->currentData().toString();
            if (idStr.isEmpty()) stage.eqPresetId = std::nullopt;
            else stage.eqPresetId = QUuid::fromString(idStr);
            applyConfig();
        });
        optsForm->addRow("Linked EQ Preset:", eqCombo);
        break;
    }

    case StageType::Convolution: {
        auto convCombo = new QComboBox(optsGroup);
        convCombo->addItem("None", QString());
        for (const auto& preset : m_pipeline->convPresets) {
            convCombo->addItem(QString::fromStdString(preset.name), preset.id.toString());
        }
        if (stage.convPresetId.has_value()) {
            int idx = convCombo->findData(stage.convPresetId->toString());
            if (idx >= 0) convCombo->setCurrentIndex(idx);
        }
        connect(convCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage, convCombo]() {
            QString idStr = convCombo->currentData().toString();
            if (idStr.isEmpty()) stage.convPresetId = std::nullopt;
            else stage.convPresetId = QUuid::fromString(idStr);
            applyConfig();
        });
        optsForm->addRow("Linked FIR Preset:", convCombo);
        break;
    }

    case StageType::Limiter: {
        auto threshSpin = new QDoubleSpinBox(optsGroup);
        threshSpin->setRange(-60.0, 0.0); threshSpin->setValue(stage.limiterThreshold); threshSpin->setSuffix(" dB");
        connect(threshSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.limiterThreshold = val; applyConfig();
        });
        optsForm->addRow("Threshold:", threshSpin);

        auto softCheck = new QCheckBox("Soft Clip Curve", optsGroup);
        softCheck->setChecked(stage.limiterSoftClip);
        connect(softCheck, &QCheckBox::toggled, [this, &stage](bool chk) { stage.limiterSoftClip = chk; applyConfig(); });
        optsForm->addRow("", softCheck);
        break;
    }

    case StageType::LookaheadLimiter: {
        auto limitSpin = new QDoubleSpinBox(optsGroup);
        limitSpin->setRange(-60.0, 0.0); limitSpin->setValue(stage.lookaheadLimit); limitSpin->setSuffix(" dB");
        connect(limitSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.lookaheadLimit = val; applyConfig();
        });
        optsForm->addRow("Peak Limit:", limitSpin);

        auto attSpin = new QDoubleSpinBox(optsGroup);
        attSpin->setRange(0.1, 100.0); attSpin->setValue(stage.lookaheadAttack); attSpin->setSuffix(" ms");
        connect(attSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.lookaheadAttack = val; applyConfig();
        });
        optsForm->addRow("Attack Time:", attSpin);

        auto relSpin = new QDoubleSpinBox(optsGroup);
        relSpin->setRange(1.0, 1000.0); relSpin->setValue(stage.lookaheadRelease); relSpin->setSuffix(" ms");
        connect(relSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.lookaheadRelease = val; applyConfig();
        });
        optsForm->addRow("Release Time:", relSpin);
        break;
    }

    case StageType::Volume: {
        auto faderCombo = new QComboBox(optsGroup);
        faderCombo->addItems({"Main", "Aux 1", "Aux 2", "Aux 3", "Aux 4"});
        faderCombo->setCurrentIndex(static_cast<int>(stage.volumeFader));
        connect(faderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.volumeFader = static_cast<Fader>(idx); applyConfig();
        });
        optsForm->addRow("Target Fader:", faderCombo);

        auto rampSpin = new QDoubleSpinBox(optsGroup);
        rampSpin->setRange(0.0, 2000.0); rampSpin->setSingleStep(50.0); rampSpin->setValue(stage.volumeRampTime); rampSpin->setSuffix(" ms");
        connect(rampSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.volumeRampTime = val; applyConfig();
        });
        optsForm->addRow("Ramp Time:", rampSpin);

        auto limitSpin = new QDoubleSpinBox(optsGroup);
        limitSpin->setRange(-50.0, 20.0); limitSpin->setSingleStep(0.5); limitSpin->setValue(stage.volumeLimit); limitSpin->setSuffix(" dB");
        connect(limitSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.volumeLimit = val; applyConfig();
        });
        optsForm->addRow("Volume Limit:", limitSpin);
        break;
    }

    case StageType::MatrixMixer: {
        auto chanDimBox = new QHBoxLayout();
        
        auto inSpin = new QSpinBox(optsGroup);
        inSpin->setRange(1, 16); inSpin->setValue(stage.mixerConfig.channelsIn > 0 ? stage.mixerConfig.channelsIn : 2);
        chanDimBox->addWidget(new QLabel("Input Channels:", optsGroup));
        chanDimBox->addWidget(inSpin);

        auto outSpin = new QSpinBox(optsGroup);
        outSpin->setRange(1, 16); outSpin->setValue(stage.mixerConfig.mapping.empty() ? 2 : static_cast<int>(stage.mixerConfig.mapping.size()));
        chanDimBox->addWidget(new QLabel("Output Channels:", optsGroup));
        chanDimBox->addWidget(outSpin);

        chanDimBox->addStretch();
        optsForm->addRow("Dimensions:", chanDimBox);

        int rows = outSpin->value();
        int cols = inSpin->value();

        auto matrixTable = new QTableWidget(rows, cols, optsGroup);
        matrixTable->horizontalHeader()->setDefaultSectionSize(120);
        matrixTable->horizontalHeader()->setMinimumSectionSize(110);
        matrixTable->verticalHeader()->setDefaultSectionSize(40);
        matrixTable->setMinimumWidth(std::min(720, cols * 125 + 50));
        matrixTable->setMinimumHeight(std::min(400, rows * 44 + 40));

        for (int r = 0; r < rows; ++r) {
            matrixTable->setRowHeight(r, 40);
            for (int c = 0; c < cols; ++c) {
                auto cellWidget = new QWidget(matrixTable);
                auto cellLayout = new QHBoxLayout(cellWidget);
                cellLayout->setContentsMargins(4, 2, 4, 2);
                cellLayout->setSpacing(2);

                auto spin = new QDoubleSpinBox(cellWidget);
                spin->setRange(-120.0, 30.0);
                spin->setSingleStep(0.5);
                spin->setSuffix("dB");
                spin->setMinimumWidth(70);

                double currentVal = 0.0;
                bool isInverted = false;
                bool isMuted = false;

                if (r < static_cast<int>(stage.mixerConfig.mapping.size())) {
                    for (const auto& src : stage.mixerConfig.mapping[r].sources) {
                        if (src.channel == c) {
                            currentVal = src.gain.value_or(0.0);
                            isInverted = src.inverted.value_or(false);
                            isMuted = src.mute.value_or(false);
                            break;
                        }
                    }
                }
                spin->setValue(currentVal);
                cellLayout->addWidget(spin, 1);

                auto invBtn = new QPushButton("Ø", cellWidget);
                invBtn->setFixedSize(22, 22);
                invBtn->setCheckable(true);
                invBtn->setChecked(isInverted);
                invBtn->setStyleSheet(isInverted ? "background-color: #ff9500; color: white; font-weight: bold; border-radius: 3px;" : "background-color: #3a3a3c; color: #8e8e93; border-radius: 3px;");
                cellLayout->addWidget(invBtn);

                auto muteBtn = new QPushButton("M", cellWidget);
                muteBtn->setFixedSize(22, 22);
                muteBtn->setCheckable(true);
                muteBtn->setChecked(isMuted);
                muteBtn->setStyleSheet(isMuted ? "background-color: #ff3b30; color: white; font-weight: bold; border-radius: 3px;" : "background-color: #3a3a3c; color: #8e8e93; border-radius: 3px;");
                cellLayout->addWidget(muteBtn);

                auto syncCellModel = [this, &stage, r, c, spin, invBtn, muteBtn]() {
                    if (r >= static_cast<int>(stage.mixerConfig.mapping.size())) stage.mixerConfig.mapping.resize(r + 1);
                    auto& map = stage.mixerConfig.mapping[r];
                    map.dest = r;
                    bool found = false;
                    for (auto& src : map.sources) {
                        if (src.channel == c) {
                            src.gain = spin->value();
                            src.inverted = invBtn->isChecked();
                            src.mute = muteBtn->isChecked();
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        MixerSource s;
                        s.channel = c;
                        s.gain = spin->value();
                        s.inverted = invBtn->isChecked();
                        s.mute = muteBtn->isChecked();
                        map.sources.push_back(s);
                    }
                    applyConfig();
                };

                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), syncCellModel);
                connect(invBtn, &QPushButton::toggled, [invBtn, syncCellModel](bool chk) {
                    invBtn->setStyleSheet(chk ? "background-color: #ff9500; color: white; font-weight: bold; border-radius: 3px;" : "background-color: #3a3a3c; color: #8e8e93; border-radius: 3px;");
                    syncCellModel();
                });
                connect(muteBtn, &QPushButton::toggled, [muteBtn, syncCellModel](bool chk) {
                    muteBtn->setStyleSheet(chk ? "background-color: #ff3b30; color: white; font-weight: bold; border-radius: 3px;" : "background-color: #3a3a3c; color: #8e8e93; border-radius: 3px;");
                    syncCellModel();
                });

                matrixTable->setCellWidget(r, c, cellWidget);
            }
        }
        optsForm->addRow("Matrix Mixer Map:", matrixTable);

        connect(inSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this, &stage](int val) {
            stage.mixerConfig.channelsIn = val;
            applyConfig();
            refreshUi();
        });
        connect(outSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this, &stage](int val) {
            stage.mixerConfig.mapping.resize(val);
            for (int i = 0; i < val; ++i) stage.mixerConfig.mapping[i].dest = i;
            applyConfig();
            refreshUi();
        });

        auto resetBtn = new QPushButton("Reset to 1:1 Passthrough", optsGroup);
        connect(resetBtn, &QPushButton::clicked, [this, &stage, rows, cols]() {
            int minCh = std::min(rows, cols);
            stage.mixerConfig.mapping.clear();
            for (int r = 0; r < rows; ++r) {
                MixerMapping m;
                m.dest = r;
                int srcCh = r < minCh ? r : 0;
                m.sources.push_back(MixerSource{srcCh, 0.0, false});
                stage.mixerConfig.mapping.push_back(m);
            }
            applyConfig();
            refreshUi();
        });
        optsForm->addRow("", resetBtn);
        break;
    }

    case StageType::Compressor: {
        auto thSpin = new QDoubleSpinBox(optsGroup);
        thSpin->setRange(-60.0, 0.0); thSpin->setValue(stage.compressorParams.threshold); thSpin->setSuffix(" dB");
        connect(thSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.compressorParams.threshold = val; applyConfig();
        });
        optsForm->addRow("Threshold:", thSpin);

        auto ratioSpin = new QDoubleSpinBox(optsGroup);
        ratioSpin->setRange(1.0, 30.0); ratioSpin->setValue(stage.compressorParams.factor);
        connect(ratioSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.compressorParams.factor = val; applyConfig();
        });
        optsForm->addRow("Ratio:", ratioSpin);

        auto attSpin = new QDoubleSpinBox(optsGroup);
        attSpin->setRange(0.1, 500.0); attSpin->setValue(stage.compressorParams.attack); attSpin->setSuffix(" ms");
        connect(attSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.compressorParams.attack = val; applyConfig();
        });
        optsForm->addRow("Attack:", attSpin);

        auto relSpin = new QDoubleSpinBox(optsGroup);
        relSpin->setRange(1.0, 5000.0); relSpin->setValue(stage.compressorParams.release); relSpin->setSuffix(" ms");
        connect(relSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.compressorParams.release = val; applyConfig();
        });
        optsForm->addRow("Release:", relSpin);

        auto mkSpin = new QDoubleSpinBox(optsGroup);
        mkSpin->setRange(0.0, 30.0); mkSpin->setValue(stage.compressorMakeupGain); mkSpin->setSuffix(" dB");
        connect(mkSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.compressorMakeupGain = val; applyConfig();
        });
        optsForm->addRow("Makeup Gain:", mkSpin);

        auto softCheck = new QCheckBox("Enable Soft Knee / Clip Curve", optsGroup);
        softCheck->setChecked(stage.compressorSoftClip);
        connect(softCheck, &QCheckBox::toggled, [this, &stage](bool chk) {
            stage.compressorSoftClip = chk; applyConfig();
        });
        optsForm->addRow("", softCheck);
        break;
    }

    case StageType::NoiseGate: {
        auto thSpin = new QDoubleSpinBox(optsGroup);
        thSpin->setRange(-90.0, 0.0); thSpin->setValue(stage.noiseGateParams.threshold); thSpin->setSuffix(" dB");
        connect(thSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.noiseGateParams.threshold = val; applyConfig();
        });
        optsForm->addRow("Threshold:", thSpin);

        auto attenSpin = new QDoubleSpinBox(optsGroup);
        attenSpin->setRange(-120.0, 0.0); attenSpin->setValue(stage.noiseGateParams.attenuation); attenSpin->setSuffix(" dB");
        connect(attenSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.noiseGateParams.attenuation = val; applyConfig();
        });
        optsForm->addRow("Attenuation:", attenSpin);

        auto attSpin = new QDoubleSpinBox(optsGroup);
        attSpin->setRange(0.1, 500.0); attSpin->setValue(stage.gateAttack); attSpin->setSuffix(" ms");
        connect(attSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.gateAttack = val; applyConfig();
        });
        optsForm->addRow("Attack Time:", attSpin);

        auto relSpin = new QDoubleSpinBox(optsGroup);
        relSpin->setRange(1.0, 5000.0); relSpin->setValue(stage.gateRelease); relSpin->setSuffix(" ms");
        connect(relSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.gateRelease = val; applyConfig();
        });
        optsForm->addRow("Release Time:", relSpin);
        break;
    }

    case StageType::RACE: {
        auto delaySpin = new QDoubleSpinBox(optsGroup);
        delaySpin->setRange(0.0, 50.0); delaySpin->setValue(stage.raceParams.delay); delaySpin->setSuffix(" cm");
        connect(delaySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.raceParams.delay = val; applyConfig();
        });
        optsForm->addRow("Speaker Distance:", delaySpin);

        auto attenSpin = new QDoubleSpinBox(optsGroup);
        attenSpin->setRange(-30.0, 0.0); attenSpin->setValue(stage.raceParams.attenuation); attenSpin->setSuffix(" dB");
        connect(attenSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.raceParams.attenuation = val; applyConfig();
        });
        optsForm->addRow("Cancellation Attenuation:", attenSpin);
        break;
    }

    case StageType::Dither: {
        auto bitsSpin = new QSpinBox(optsGroup);
        bitsSpin->setRange(8, 32); bitsSpin->setValue(stage.ditherBits);
        connect(bitsSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this, &stage](int val) {
            stage.ditherBits = val; applyConfig();
        });
        optsForm->addRow("Dither Target Bit Depth:", bitsSpin);
        break;
    }

    case StageType::DiffEq: {
        auto vecToString = [](const std::vector<double>& v) {
            QStringList list;
            for (double val : v) list << QString::number(val);
            return list.join(", ");
        };
        auto stringToVec = [](const QString& str) {
            std::vector<double> v;
            for (const QString& part : str.split(',')) {
                bool ok = false;
                double d = part.trimmed().toDouble(&ok);
                if (ok) v.push_back(d);
            }
            return v;
        };

        auto bEdit = new QLineEdit(vecToString(stage.diffEqB), optsGroup);
        bEdit->setPlaceholderText("e.g. 1.0, 0.5, 0.25");
        connect(bEdit, &QLineEdit::editingFinished, [this, &stage, bEdit, stringToVec]() {
            stage.diffEqB = stringToVec(bEdit->text()); applyConfig();
        });
        optsForm->addRow("Feedforward Coeffs (b):", bEdit);

        auto aEdit = new QLineEdit(vecToString(stage.diffEqA), optsGroup);
        aEdit->setPlaceholderText("e.g. 1.0, -0.5, 0.1");
        connect(aEdit, &QLineEdit::editingFinished, [this, &stage, aEdit, stringToVec]() {
            stage.diffEqA = stringToVec(aEdit->text()); applyConfig();
        });
        optsForm->addRow("Feedback Coeffs (a):", aEdit);
        break;
    }

    case StageType::BiquadCombo: {
        auto typeCombo = new QComboBox(optsGroup);
        typeCombo->addItems({"Butterworth Lowpass", "Butterworth Highpass", "Linkwitz-Riley Lowpass", "Linkwitz-Riley Highpass", "Tilt", "Five-Point PEQ"});
        typeCombo->setCurrentIndex(static_cast<int>(stage.comboParams.type));
        connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, &stage](int idx) {
            stage.comboParams.type = static_cast<BiquadComboType>(idx); applyConfig();
        });
        optsForm->addRow("Combination Type:", typeCombo);

        auto freqSpin = new QDoubleSpinBox(optsGroup);
        freqSpin->setRange(20.0, 20000.0); freqSpin->setValue(stage.comboParams.freq.value_or(1000.0)); freqSpin->setSuffix(" Hz");
        connect(freqSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.comboParams.freq = val; applyConfig();
        });
        optsForm->addRow("Cutoff / Center Freq:", freqSpin);

        auto orderSpin = new QSpinBox(optsGroup);
        orderSpin->setRange(2, 8); orderSpin->setSingleStep(2); orderSpin->setValue(stage.comboParams.order.value_or(2));
        connect(orderSpin, QOverload<int>::of(&QSpinBox::valueChanged), [this, &stage](int val) {
            stage.comboParams.order = val; applyConfig();
        });
        optsForm->addRow("Filter Order:", orderSpin);

        auto tiltSpin = new QDoubleSpinBox(optsGroup);
        tiltSpin->setRange(-12.0, 12.0); tiltSpin->setValue(stage.comboParams.gain.value_or(0.0)); tiltSpin->setSuffix(" dB");
        connect(tiltSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [this, &stage](double val) {
            stage.comboParams.gain = val; applyConfig();
        });
        optsForm->addRow("Tilt Slope Gain:", tiltSpin);
        break;
    }

    default: {
        auto infoLbl = new QLabel("General stage parameters configured.", optsGroup);
        optsForm->addRow("", infoLbl);
        break;
    }
    }

    containerLayout->addWidget(optsGroup);
}
