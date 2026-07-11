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

    // Defer option container rebuild to prevent combobox destruction crash
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
        optsForm->addRow(new QLabel("31-Band Graphic EQ Gains (-12 dB to +12 dB)", optsGroup));
        break;
    }

    case StageType::Loudness: {
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
        auto deCheck = new QCheckBox("De-Emphasis Filter (50μs/15μs, -9.5 dB at 5.2 kHz)", optsGroup);
        deCheck->setChecked(stage.deEmphasis);
        connect(deCheck, &QCheckBox::toggled, [this, &stage](bool chk) { stage.deEmphasis = chk; applyConfig(); });
        optsForm->addRow("", deCheck);
        break;
    }

    case StageType::DCProtection: {
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
        optsForm->addRow(new QLabel("Main System Volume Control Fader", optsGroup));
        break;
    }

    case StageType::MatrixMixer: {
        int rows = stage.mixerConfig.mapping.size();
        int cols = stage.mixerConfig.channelsIn;
        if (rows == 0) rows = 2;
        if (cols == 0) cols = 2;

        auto matrixTable = new QTableWidget(rows, cols, optsGroup);
        matrixTable->setMinimumHeight(200);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                auto spin = new QDoubleSpinBox(optsGroup);
                spin->setRange(-120.0, 30.0);
                spin->setSingleStep(0.5);
                spin->setValue(0.0);
                matrixTable->setCellWidget(r, c, spin);
            }
        }
        optsForm->addRow("Matrix Mixer Map:", matrixTable);
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
        optsForm->addRow(new QLabel("Custom Differential Equation Filter Coefficients", optsGroup));
        break;
    }

    case StageType::BiquadCombo: {
        optsForm->addRow(new QLabel("Biquad Combination Filter Set", optsGroup));
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
