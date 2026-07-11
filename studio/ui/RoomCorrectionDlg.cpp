#include "ui/RoomCorrectionDlg.h"
#include "ui/StyleTheme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>

RoomCorrectionDlg::RoomCorrectionDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Room Correction Studio & PEQ Auto-Fit");
    resize(1000, 720);
    setupUi();

    connect(&m_session, &MeasurementSession::sessionUpdated, this, &RoomCorrectionDlg::refreshSessionUi);
    refreshSessionUi();
}

void RoomCorrectionDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    m_tabWidget = new QTabWidget(this);

    auto measTab = new QWidget(this); setupMeasurementTab(measTab); m_tabWidget->addTab(measTab, "Measurements & FR");
    auto fitTab = new QWidget(this); setupFitTab(fitTab); m_tabWidget->addTab(fitTab, "PEQ Auto-Fit");
    auto subTab = new QWidget(this); setupSubwooferTab(subTab); m_tabWidget->addTab(subTab, "Subwoofer Crossover Assist");
    auto waterfallTab = new QWidget(this); setupWaterfallTab(waterfallTab); m_tabWidget->addTab(waterfallTab, "CSD Waterfall");
    auto firTab = new QWidget(this); setupFIRTab(firTab); m_tabWidget->addTab(firTab, "FIR Export");

    mainLayout->addWidget(m_tabWidget);

    auto bottomLayout = new QHBoxLayout();
    m_statusLabel = new QLabel("Ready.", this);
    bottomLayout->addWidget(m_statusLabel);
    bottomLayout->addStretch();

    auto closeBtn = new QPushButton("Close Workspace", this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(closeBtn);

    mainLayout->addLayout(bottomLayout);
}

void RoomCorrectionDlg::setupMeasurementTab(QWidget* tab) {
    auto layout = new QHBoxLayout(tab);

    // Left controls & positions list
    auto leftBox = new QVBoxLayout();

    auto mockBtn = new QPushButton("Generate Mock Measurement", tab);
    connect(mockBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onGenerateMock);
    leftBox->addWidget(mockBtn);

    auto impBtn = new QPushButton("Import REW FRD File", tab);
    connect(impBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onImportFRD);
    leftBox->addWidget(impBtn);

    auto expBtn = new QPushButton("Export FRD File", tab);
    connect(expBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onExportFRD);
    leftBox->addWidget(expBtn);

    leftBox->addWidget(new QLabel("Captured Positions:", tab));
    m_positionsList = new QListWidget(tab);
    leftBox->addWidget(m_positionsList);

    layout->addLayout(leftBox, 1);

    // Right FR Diagram
    m_frDiagramWidget = new EQDiagramWidget(tab);
    layout->addWidget(m_frDiagramWidget, 3);
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

void RoomCorrectionDlg::setupWaterfallTab(QWidget* tab) {
    auto layout = new QVBoxLayout(tab);
    m_waterfallWidget = new WaterfallPlotWidget(tab);
    layout->addWidget(m_waterfallWidget);
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

    m_smoothingCombo = new QComboBox(tab);
    m_smoothingCombo->addItems({"No Smoothing", "1/48 Octave", "1/24 Octave", "1/12 Octave", "1/6 Octave", "1/3 Octave", "ERB"});
    m_smoothingCombo->setCurrentIndex(4);
    connect(m_smoothingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.displaySmoothing = static_cast<DisplaySmoothing>(idx);
        refreshSessionUi();
    });
    form->addRow("Display Smoothing:", m_smoothingCombo);

    layout->addLayout(form);

    auto genBtn = new QPushButton("Generate Multi-Rate FIR Preset", tab);
    connect(genBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onGenerateFIR);
    layout->addWidget(genBtn);

    layout->addStretch();
}

void RoomCorrectionDlg::refreshSessionUi() {
    m_statusLabel->setText(QString::fromStdString(m_session.status));

    m_positionsList->clear();
    for (const auto& p : m_session.positions) {
        m_positionsList->addItem(QString::fromStdString(p.name + " (" + channelKindToString(p.kind) + ")"));
    }

    if (m_session.correctionPreset.has_value()) {
        m_frDiagramWidget->setPreset(m_session.correctionPreset.value());
    }

    if (m_session.measuredIR.has_value()) {
        auto stftSlices = FrequencyResponse::stft(m_session.measuredIR.value());
        m_waterfallWidget->setSlices(stftSlices);
    }
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
        m_subResultLabel->setText(QString("Subwoofer Recommendation:\n• Delay Offset: %1 ms (%2 samples)\n• Recommended Crossover: %3 Hz\n• Invert Sub Phase: %4")
            .arg(rec->delayMs, 0, 'f', 2)
            .arg(rec->delaySamples)
            .arg(rec->recommendedCrossoverHz, 0, 'f', 0)
            .arg(rec->invertSubPhase ? "YES" : "NO"));
    } else {
        m_subResultLabel->setText("Could not compute recommendation. Ensure one Mains and one Subwoofer position are loaded.");
    }
}
