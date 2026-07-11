#include "ui/RoomCorrectionDlg.h"
#include "ui/StyleTheme.h"
#include "room_correction/CalibrationCurve.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>

RoomCorrectionDlg::RoomCorrectionDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Room Correction Studio & Multi-Plot Analyzer");
    resize(1100, 760);
    setupUi();

    connect(&m_session, &MeasurementSession::sessionUpdated, this, &RoomCorrectionDlg::refreshSessionUi);
    refreshSessionUi();
}

void RoomCorrectionDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    m_tabWidget = new QTabWidget(this);

    auto measTab = new QWidget(this); setupMeasurementTab(measTab); m_tabWidget->addTab(measTab, "Measurements & Analysis");
    auto fitTab = new QWidget(this); setupFitTab(fitTab); m_tabWidget->addTab(fitTab, "PEQ Auto-Fit");
    auto subTab = new QWidget(this); setupSubwooferTab(subTab); m_tabWidget->addTab(subTab, "Subwoofer Crossover Assist");
    auto firTab = new QWidget(this); setupFIRTab(firTab); m_tabWidget->addTab(firTab, "FIR Export");

    mainLayout->addWidget(m_tabWidget);

    auto bottomLayout = new QHBoxLayout();
    m_statusLabel = new QLabel("Ready.", this);
    bottomLayout->addWidget(m_statusLabel);
    bottomLayout->addStretch();

    auto closeBtn = new QPushButton("Close Studio", this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(closeBtn);

    mainLayout->addLayout(bottomLayout);
}

void RoomCorrectionDlg::setupMeasurementTab(QWidget* tab) {
    auto mainMeasLayout = new QVBoxLayout(tab);

    // Multi-Plot Tab Bar at the top of the measurement view
    m_plotTabBar = new QTabBar(tab);
    m_plotTabBar->setExpanding(false);
    m_plotTabBar->addTab("Magnitude (dB / Freq)");
    m_plotTabBar->addTab("Phase Plot");
    m_plotTabBar->addTab("Impulse Response");
    m_plotTabBar->addTab("Group Delay");
    m_plotTabBar->addTab("3D Waterfall (CSD)");

    mainMeasLayout->addWidget(m_plotTabBar);

    auto contentLayout = new QHBoxLayout();

    // Left controls & positions list
    auto leftBox = new QVBoxLayout();
    leftBox->setSpacing(8);

    auto mockBtn = new QPushButton("Generate Mock Measurement", tab);
    connect(mockBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onGenerateMock);
    leftBox->addWidget(mockBtn);

    auto impBtn = new QPushButton("Import REW FRD File", tab);
    connect(impBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onImportFRD);
    leftBox->addWidget(impBtn);

    auto expBtn = new QPushButton("Export FRD File", tab);
    connect(expBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onExportFRD);
    leftBox->addWidget(expBtn);

    // Mic Calibration file controls
    leftBox->addWidget(new QLabel("Microphone Calibration:", tab));
    m_calStatusLabel = new QLabel("None loaded", tab);
    m_calStatusLabel->setWordWrap(true);
    m_calStatusLabel->setStyleSheet("color: #aaa; font-size: 11px;");
    leftBox->addWidget(m_calStatusLabel);

    auto calBtnLayout = new QHBoxLayout();
    auto loadCalBtn = new QPushButton("Load Cal…", tab);
    connect(loadCalBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onLoadCalibration);
    calBtnLayout->addWidget(loadCalBtn);

    auto clearCalBtn = new QPushButton("Clear Cal", tab);
    connect(clearCalBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onClearCalibration);
    calBtnLayout->addWidget(clearCalBtn);
    leftBox->addLayout(calBtnLayout);

    // Analysis Settings (FDW & Smoothing)
    leftBox->addWidget(new QLabel("Analysis & Display Settings:", tab));
    auto fdwForm = new QFormLayout();

    m_fdwCombo = new QComboBox(tab);
    m_fdwCombo->addItems({"Off", "1 Cycle", "5 Cycles", "10 Cycles", "15 Cycles"});
    connect(m_fdwCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        switch (idx) {
        case 0: m_session.fdwCycles = FDWCycles::Off; break;
        case 1: m_session.fdwCycles = FDWCycles::Cycles1; break;
        case 2: m_session.fdwCycles = FDWCycles::Cycles5; break;
        case 3: m_session.fdwCycles = FDWCycles::Cycles10; break;
        case 4: m_session.fdwCycles = FDWCycles::Cycles15; break;
        }
        m_session.recomputeAverage();
    });
    fdwForm->addRow("FDW Window:", m_fdwCombo);

    m_smoothingCombo = new QComboBox(tab);
    m_smoothingCombo->addItems({"No Smoothing", "1/1 Octave", "1/3 Octave", "1/6 Octave", "1/12 Octave", "1/24 Octave", "1/48 Octave", "Var (ERB)"});
    m_smoothingCombo->setCurrentIndex(3); // Default 1/6 oct
    connect(m_smoothingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.displaySmoothing = static_cast<DisplaySmoothing>(idx);
        refreshSessionUi();
    });
    fdwForm->addRow("Smoothing:", m_smoothingCombo);

    leftBox->addLayout(fdwForm);

    leftBox->addWidget(new QLabel("Captured Positions:", tab));
    m_positionsList = new QListWidget(tab);
    leftBox->addWidget(m_positionsList);

    contentLayout->addLayout(leftBox, 1);

    // Right Multi-Plot Stacked Widget
    m_plotStackedWidget = new QStackedWidget(tab);

    m_frDiagramWidget = new EQDiagramWidget(tab);
    m_plotStackedWidget->addWidget(m_frDiagramWidget);

    m_phasePlotWidget = new PhasePlotWidget(tab);
    m_phasePlotWidget->setSession(&m_session);
    m_plotStackedWidget->addWidget(m_phasePlotWidget);

    m_impulsePlotWidget = new ImpulseResponsePlotWidget(tab);
    m_impulsePlotWidget->setSession(&m_session);
    m_plotStackedWidget->addWidget(m_impulsePlotWidget);

    m_groupDelayPlotWidget = new GroupDelayPlotWidget(tab);
    m_groupDelayPlotWidget->setSession(&m_session);
    m_plotStackedWidget->addWidget(m_groupDelayPlotWidget);

    m_waterfallWidget = new WaterfallPlotWidget(tab);
    m_plotStackedWidget->addWidget(m_waterfallWidget);

    connect(m_plotTabBar, &QTabBar::currentChanged, m_plotStackedWidget, &QStackedWidget::setCurrentIndex);

    contentLayout->addWidget(m_plotStackedWidget, 3);
    mainMeasLayout->addLayout(contentLayout);
}

void RoomCorrectionDlg::setupFitTab(QWidget* tab) {
    auto layout = new QVBoxLayout(tab);
    auto form = new QFormLayout();

    m_bandCountSpin = new QSpinBox(tab);
    m_bandCountSpin->setRange(1, 20); m_bandCountSpin->setValue(8);
    form->addRow("Band Count:", m_bandCountSpin);

    m_maxGainSpin = new QDoubleSpinBox(tab);
    m_maxGainSpin->setRange(1.0, 24.0); m_maxGainSpin->setValue(12.0);
    form->addRow("Max Gain Cap (dB):", m_maxGainSpin);

    m_modalCheck = new QCheckBox("Modal Region Optimization (Cuts Only below Schroeder)", tab);
    form->addRow("", m_modalCheck);

    m_schroederSpin = new QDoubleSpinBox(tab);
    m_schroederSpin->setRange(50.0, 500.0); m_schroederSpin->setValue(200.0);
    form->addRow("Schroeder Frequency (Hz):", m_schroederSpin);

    m_targetPresetCombo = new QComboBox(tab);
    m_targetPresetCombo->addItems({"Flat (0 dB)", "Brüel & Kjær House Curve", "Harman In-Room Target"});
    form->addRow("Target House Curve:", m_targetPresetCombo);

    auto targetFileBtn = new QPushButton("Load Custom Target Curve File…", tab);
    connect(targetFileBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onLoadTargetCurve);
    form->addRow("", targetFileBtn);

    layout->addLayout(form);

    auto fitBtn = new QPushButton("Run PEQ Auto-Fit Optimization", tab);
    connect(fitBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onRunFit);
    layout->addWidget(fitBtn);

    auto applyBtn = new QPushButton("Apply Fitted EQ to Active Pipeline", tab);
    connect(applyBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onApplyEQToPipeline);
    layout->addWidget(applyBtn);

    layout->addStretch();
}

void RoomCorrectionDlg::setupSubwooferTab(QWidget* tab) {
    auto layout = new QVBoxLayout(tab);

    auto computeBtn = new QPushButton("Compute Time-of-Flight & Crossover", tab);
    connect(computeBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onComputeSubwoofer);
    layout->addWidget(computeBtn);

    m_subResultLabel = new QLabel("Load one Mains position and one Subwoofer position to run assistant.", tab);
    m_subResultLabel->setWordWrap(true);
    m_subResultLabel->setFont(QFont("sans-serif", 13));
    layout->addWidget(m_subResultLabel);

    layout->addStretch();
}

void RoomCorrectionDlg::setupFIRTab(QWidget* tab) {
    auto layout = new QVBoxLayout(tab);
    auto form = new QFormLayout();

    m_firKindCombo = new QComboBox(tab);
    m_firKindCombo->addItems({"Min-phase", "Linear-phase", "From measurement"});
    form->addRow("FIR Filter Kind:", m_firKindCombo);

    m_firTapSpin = new QSpinBox(tab);
    m_firTapSpin->setRange(1024, 65536); m_firTapSpin->setValue(8192); m_firTapSpin->setSingleStep(1024);
    form->addRow("FIR Tap Count (FFT Size):", m_firTapSpin);

    auto blendLayout = new QHBoxLayout();
    m_firPhaseBlendSlider = new QSlider(Qt::Horizontal, tab);
    m_firPhaseBlendSlider->setRange(0, 100);
    m_firPhaseBlendSlider->setValue(0);
    blendLayout->addWidget(m_firPhaseBlendSlider);

    m_firPhaseBlendLabel = new QLabel("Min-phase (0%)", tab);
    m_firPhaseBlendLabel->setFixedWidth(120);
    blendLayout->addWidget(m_firPhaseBlendLabel);

    connect(m_firPhaseBlendSlider, &QSlider::valueChanged, [this](int val) {
        double blend = val / 100.0;
        m_session.firPhaseBlend = blend;
        if (val == 0) m_firPhaseBlendLabel->setText("Min-phase (0%)");
        else if (val == 100) m_firPhaseBlendLabel->setText("Linear-phase (100%)");
        else m_firPhaseBlendLabel->setText(QString("%1% Blend").arg(val));
    });
    form->addRow("Phase Blend (Measurement):", blendLayout);

    layout->addLayout(form);

    auto genBtn = new QPushButton("Generate Multi-Rate FIR Preset", tab);
    connect(genBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onGenerateFIR);
    layout->addWidget(genBtn);

    layout->addStretch();
}

void RoomCorrectionDlg::refreshSessionUi() {
    m_statusLabel->setText(QString::fromStdString(m_session.status));

    // Update position chips list with row widgets
    m_positionsList->clear();
    for (const auto& p : m_session.positions) {
        auto item = new QListWidgetItem(m_positionsList);
        auto rowWidget = new MeasurementPositionRowWidget(p, &m_session, this);
        item->setSizeHint(rowWidget->sizeHint());
        m_positionsList->setItemWidget(item, rowWidget);
    }

    // Update calibration label
    if (m_session.calibrationPath.empty()) {
        m_calStatusLabel->setText("None loaded");
    } else {
        QFileInfo fi(QString::fromStdString(m_session.calibrationPath));
        m_calStatusLabel->setText("Loaded: " + fi.fileName());
    }

    // Refresh plot widgets
    if (m_session.correctionPreset.has_value()) {
        m_frDiagramWidget->setPreset(m_session.correctionPreset.value());
    }

    if (m_session.measuredIR.has_value()) {
        auto stftSlices = FrequencyResponse::stft(m_session.measuredIR.value());
        m_waterfallWidget->setSlices(stftSlices);
    }

    m_phasePlotWidget->update();
    m_impulsePlotWidget->update();
    m_groupDelayPlotWidget->update();
}

void RoomCorrectionDlg::onGenerateMock() {
    m_session.generateMockMeasurement(false);
}

void RoomCorrectionDlg::onImportFRD() {
    QString path = QFileDialog::getOpenFileName(this, "Import REW FRD", "", "FRD Files (*.frd *.txt)");
    if (!path.isEmpty()) {
        m_session.importPositionFRD(path.toStdString());
    }
}

void RoomCorrectionDlg::onExportFRD() {
    QString path = QFileDialog::getSaveFileName(this, "Export REW FRD", "", "FRD Files (*.frd *.txt)");
    if (!path.isEmpty()) {
        m_session.exportFRD(path.toStdString(), true);
    }
}

void RoomCorrectionDlg::onLoadCalibration() {
    QString path = QFileDialog::getOpenFileName(this, "Load Microphone Calibration File", "", "Calibration Files (*.frd *.txt *.cal)");
    if (!path.isEmpty()) {
        m_session.loadCalibration(path.toStdString());
    }
}

void RoomCorrectionDlg::onClearCalibration() {
    m_session.clearCalibration();
}

void RoomCorrectionDlg::onLoadTargetCurve() {
    QString path = QFileDialog::getOpenFileName(this, "Load Target Curve File", "", "Target Curve Files (*.frd *.txt *.csv)");
    if (!path.isEmpty()) {
        auto cal = CalibrationCurve::load(path.toStdString());
        if (cal.has_value()) {
            TargetCurve tc;
            for (size_t i = 0; i < cal->frequencies.size(); ++i) {
                tc.breakpoints.push_back({cal->frequencies[i], cal->magnitudesDB[i]});
            }
            m_session.customTarget = tc;
            m_session.recomputeAverage();
            QMessageBox::information(this, "Target Loaded", "Loaded custom target curve successfully!");
        } else {
            QMessageBox::warning(this, "Target Load Failed", "Could not parse target curve file.");
        }
    }
}

void RoomCorrectionDlg::onRunFit() {
    m_session.bandCount = m_bandCountSpin->value();
    m_session.maxGainDB = m_maxGainSpin->value();
    m_session.modalMode = m_modalCheck->isChecked();
    m_session.schroederHz = m_schroederSpin->value();

    switch (m_targetPresetCombo->currentIndex()) {
    case 0: m_session.targetPreset = TargetPreset::Flat; break;
    case 1: m_session.targetPreset = TargetPreset::BruelKjaer; break;
    case 2: m_session.targetPreset = TargetPreset::Harman; break;
    }

    m_session.runFit();
}

void RoomCorrectionDlg::onApplyEQToPipeline() {
    if (m_session.correctionPreset.has_value()) {
        m_pipeline->addEQPreset(m_session.correctionPreset.value());
        QMessageBox::information(this, "Applied", "Fitted PEQ preset applied to pipeline!");
    }
}

void RoomCorrectionDlg::onGenerateFIR() {
    m_session.firTapCount = m_firTapSpin->value();
    switch (m_firKindCombo->currentIndex()) {
    case 0: m_session.firKind = FIRKind::MinimumPhase; break;
    case 1: m_session.firKind = FIRKind::LinearPhase; break;
    case 2: m_session.firKind = FIRKind::MeasurementDriven; break;
    }

    std::vector<std::string> names;
    for (const auto& p : m_pipeline->convPresets) names.push_back(p.name);

    auto preset = m_session.generateFIR(names);
    if (preset.has_value()) {
        m_pipeline->addConvPreset(preset.value());
        QMessageBox::information(this, "Success", "FIR Preset generated and added to Convolution presets!");
    }
}

void RoomCorrectionDlg::onComputeSubwoofer() {
    auto rec = m_session.computeSubwooferRecommendation();
    if (rec.has_value()) {
        m_subResultLabel->setText(QString("Subwoofer Recommendation:\n• Sub Delay Offset: %1 ms (%2 samples)\n• Recommended Crossover: %3 Hz\n• Confidence: %4%\n\nRationale:\n%5")
            .arg(rec->subDelayMs, 0, 'f', 2)
            .arg(rec->delaySamples)
            .arg(rec->crossoverHz, 0, 'f', 0)
            .arg(rec->confidence * 100.0, 0, 'f', 0)
            .arg(QString::fromStdString(rec->summary)));
    } else {
        m_subResultLabel->setText("Could not compute recommendation. Ensure one Mains and one Subwoofer position are loaded.");
    }
}
