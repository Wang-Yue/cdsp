#include "ui/RoomCorrectionDlg.h"

#include "room_correction/CalibrationCurve.h"
#include "ui/StyleTheme.h"

#include <QEventLoop>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QStandardPaths>

#ifdef QT_MULTIMEDIA_LIB
#include <QAudioDevice>
#include <QMediaDevices>
#endif

RoomCorrectionDlg::RoomCorrectionDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Room Correction Studio");
    resize(1150, 760);
    setupUi();

    connect(&m_session, &MeasurementSession::sessionUpdated, this, &RoomCorrectionDlg::refreshSessionUi);
    refreshSessionUi();
}

void RoomCorrectionDlg::setupUi() {
    auto mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Top Header Toolbar
    mainLayout->addWidget(createHeaderToolbar());

    // Horizontal Line Divider
    auto topDivider = new QFrame(this);
    topDivider->setFrameShape(QFrame::HLine);
    topDivider->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(topDivider);

    // Main Content Area (Plots + Positions Bar + Sidebar)
    mainLayout->addWidget(createMainArea(), 1);

    // Bottom Divider
    auto bottomDivider = new QFrame(this);
    bottomDivider->setFrameShape(QFrame::HLine);
    bottomDivider->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(bottomDivider);

    // Bottom Status Bar
    auto statusBar = new QWidget(this);
    auto statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(12, 6, 12, 6);
    statusLayout->setSpacing(8);

    auto infoIcon = new QLabel("ℹ", statusBar);
    infoIcon->setStyleSheet("color: #888; font-size: 12px;");
    statusLayout->addWidget(infoIcon);

    m_statusLabel = new QLabel("Ready.", statusBar);
    m_statusLabel->setStyleSheet("color: #aaa; font-size: 11px;");
    statusLayout->addWidget(m_statusLabel, 1);

    mainLayout->addWidget(statusBar);

    populateAudioDevices();
}

QWidget* RoomCorrectionDlg::createHeaderToolbar() {
    auto toolbar = new QWidget(this);
    auto layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);

    // Close button
    auto closeBtn = new QToolButton(toolbar);
    closeBtn->setText("✕");
    closeBtn->setToolTip("Close");
    closeBtn->setStyleSheet(
        "QToolButton { border: none; font-size: 16px; color: #888; } QToolButton:hover { color: #fff; }");
    connect(closeBtn, &QToolButton::clicked, this, &QDialog::accept);
    layout->addWidget(closeBtn);

    // Measure Dropdown Menu Button
    m_measureMenuBtn = new QPushButton("Measure ▾", toolbar);
    auto measureMenu = new QMenu(m_measureMenuBtn);

    auto realSection = measureMenu->addSection("Real measurement");
    (void)realSection;
    auto newCapAction = measureMenu->addAction("🎤 New Capture");
    connect(newCapAction, &QAction::triggered, [this]() { onRecordHardwareMeasurement(false); });

    auto addCapAction = measureMenu->addAction("➕ Add Capture as Position");
    connect(addCapAction, &QAction::triggered, [this]() { onRecordHardwareMeasurement(true); });

    measureMenu->addSection("Mock");
    auto newMockAction = measureMenu->addAction("🎲 New Mock Measurement");
    connect(newMockAction, &QAction::triggered, [this]() { onGenerateMock(false); });

    auto addMockAction = measureMenu->addAction("➕ Add Mock Position");
    connect(addMockAction, &QAction::triggered, [this]() { onGenerateMock(true); });

    measureMenu->addSection("Import");
    auto importAction = measureMenu->addAction("📥 Import FRD as Position…");
    connect(importAction, &QAction::triggered, this, &RoomCorrectionDlg::onImportFRD);

    m_measureMenuBtn->setMenu(measureMenu);
    layout->addWidget(m_measureMenuBtn);

    layout->addStretch(1);

    // Pane Picker Segmented TabBar
    m_paneTabBar = new QTabBar(toolbar);
    m_paneTabBar->setExpanding(false);
    m_paneTabBar->addTab("Magnitude");
    m_paneTabBar->addTab("Phase");
    m_paneTabBar->addTab("Impulse");
    m_paneTabBar->addTab("Group Delay");
    m_paneTabBar->addTab("Waterfall (CSD)");
    connect(m_paneTabBar, &QTabBar::currentChanged, [this](int idx) { m_plotStackedWidget->setCurrentIndex(idx); });
    layout->addWidget(m_paneTabBar);

    layout->addStretch(1);

    // Sidebar Toggle Button
    m_sidebarToggleBtn = new QToolButton(toolbar);
    m_sidebarToggleBtn->setText("▤");
    m_sidebarToggleBtn->setToolTip("Toggle Sidebar");
    m_sidebarToggleBtn->setStyleSheet(
        "QToolButton { border: none; font-size: 16px; color: #aaa; } QToolButton:hover { color: #fff; }");
    connect(m_sidebarToggleBtn, &QToolButton::clicked, this, &RoomCorrectionDlg::toggleSidebar);
    layout->addWidget(m_sidebarToggleBtn);

    return toolbar;
}

QWidget* RoomCorrectionDlg::createMainArea() {
    auto mainWidget = new QWidget(this);
    auto mainLayout = new QHBoxLayout(mainWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Left Panel (Plot Stacked Widget + Divider + Positions Bar)
    auto leftWidget = new QWidget(mainWidget);
    auto leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    m_plotStackedWidget = new QStackedWidget(leftWidget);

    m_frDiagramWidget = new EQDiagramWidget(leftWidget);
    m_plotStackedWidget->addWidget(m_frDiagramWidget);

    m_phasePlotWidget = new PhasePlotWidget(leftWidget);
    m_phasePlotWidget->setSession(&m_session);
    m_plotStackedWidget->addWidget(m_phasePlotWidget);

    m_impulsePlotWidget = new ImpulseResponsePlotWidget(leftWidget);
    m_impulsePlotWidget->setSession(&m_session);
    m_plotStackedWidget->addWidget(m_impulsePlotWidget);

    m_groupDelayPlotWidget = new GroupDelayPlotWidget(leftWidget);
    m_groupDelayPlotWidget->setSession(&m_session);
    m_plotStackedWidget->addWidget(m_groupDelayPlotWidget);

    m_waterfallWidget = new WaterfallPlotWidget(leftWidget);
    m_plotStackedWidget->addWidget(m_waterfallWidget);

    leftLayout->addWidget(m_plotStackedWidget, 1);

    auto posDivider = new QFrame(leftWidget);
    posDivider->setFrameShape(QFrame::HLine);
    posDivider->setFrameShadow(QFrame::Sunken);
    leftLayout->addWidget(posDivider);

    // Positions Bar
    m_positionsContainer = new QWidget(leftWidget);
    auto posBarLayout = new QHBoxLayout(m_positionsContainer);
    posBarLayout->setContentsMargins(12, 6, 12, 6);
    posBarLayout->setSpacing(10);

    auto posLabel = new QLabel("Positions", m_positionsContainer);
    posLabel->setStyleSheet("font-weight: bold; font-size: 11px; color: #888;");
    posBarLayout->addWidget(posLabel);

    auto scrollArea = new QScrollArea(m_positionsContainer);
    scrollArea->setWidgetResizable(true);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setFixedHeight(42);

    auto chipsWidget = new QWidget(scrollArea);
    m_positionsChipsLayout = new QHBoxLayout(chipsWidget);
    m_positionsChipsLayout->setContentsMargins(0, 0, 0, 0);
    m_positionsChipsLayout->setSpacing(8);
    m_positionsChipsLayout->addStretch(1);
    chipsWidget->setLayout(m_positionsChipsLayout);

    scrollArea->setWidget(chipsWidget);
    posBarLayout->addWidget(scrollArea, 1);

    m_subwooferAssistBtn = new QPushButton("Subwoofer Assist", m_positionsContainer);
    m_subwooferAssistBtn->setStyleSheet("QPushButton { font-size: 11px; padding: 4px 8px; }");
    connect(m_subwooferAssistBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onComputeSubwoofer);
    m_subwooferAssistBtn->setVisible(false);
    posBarLayout->addWidget(m_subwooferAssistBtn);

    leftLayout->addWidget(m_positionsContainer);

    mainLayout->addWidget(leftWidget, 1);

    // Vertical Divider before Sidebar
    auto sideDivider = new QFrame(mainWidget);
    sideDivider->setFrameShape(QFrame::VLine);
    sideDivider->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(sideDivider);

    // Right Sidebar
    m_sidebarWidget = createSidebar();
    m_sidebarWidget->setFixedWidth(290);
    mainLayout->addWidget(m_sidebarWidget);

    return mainWidget;
}

QWidget* RoomCorrectionDlg::createSidebar() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto content = new QWidget(scroll);
    auto layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(14);

    // 1. Audio Setup Section
    auto audioBox = new QWidget(content);
    auto audioLayout = new QVBoxLayout(audioBox);
    audioLayout->setContentsMargins(0, 0, 0, 0);
    audioLayout->setSpacing(6);

    audioLayout->addWidget(new QLabel("Microphone Input", audioBox));
    m_micDeviceCombo = new QComboBox(audioBox);
    m_micDeviceCombo->addItem("System Default", "");
    connect(m_micDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.selectedMicName = m_micDeviceCombo->itemData(idx).toString().toStdString();
        updateMicChannels();
    });
    audioLayout->addWidget(m_micDeviceCombo);

    m_micChannelCombo = new QComboBox(audioBox);
    m_micChannelCombo->addItem("Channel 1", 0);
    connect(m_micChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int idx) { m_session.selectedInputChannel = m_micChannelCombo->itemData(idx).toInt(); });
    audioLayout->addWidget(m_micChannelCombo);

    audioLayout->addWidget(new QLabel("Speaker Output", audioBox));
    m_outputDeviceCombo = new QComboBox(audioBox);
    m_outputDeviceCombo->addItem("System Default", "");
    connect(m_outputDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.selectedOutputName = m_outputDeviceCombo->itemData(idx).toString().toStdString();
        updateOutputChannels();
    });
    audioLayout->addWidget(m_outputDeviceCombo);

    m_outputChannelCombo = new QComboBox(audioBox);
    m_outputChannelCombo->addItem("All channels", -1);
    connect(m_outputChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int idx) { m_session.selectedOutputChannel = m_outputChannelCombo->itemData(idx).toInt(); });
    audioLayout->addWidget(m_outputChannelCombo);

    audioLayout->addWidget(new QLabel("Calibration File", audioBox));
    auto calRow = new QHBoxLayout();
    m_calPathLabel = new QLabel("None loaded", audioBox);
    m_calPathLabel->setStyleSheet("color: #888; font-size: 11px;");
    calRow->addWidget(m_calPathLabel, 1);

    m_loadCalBtn = new QPushButton("Load…", audioBox);
    connect(m_loadCalBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onLoadCalibration);
    calRow->addWidget(m_loadCalBtn);

    m_clearCalBtn = new QToolButton(audioBox);
    m_clearCalBtn->setText("✕");
    m_clearCalBtn->setVisible(false);
    connect(m_clearCalBtn, &QToolButton::clicked, this, &RoomCorrectionDlg::onClearCalibration);
    calRow->addWidget(m_clearCalBtn);
    audioLayout->addLayout(calRow);

    audioLayout->addWidget(new QLabel("Export Data", audioBox));
    auto expRow = new QHBoxLayout();
    m_exportFrdBtn = new QPushButton("Export FRD", audioBox);
    connect(m_exportFrdBtn, &QPushButton::clicked, [this]() { onExportFRD(false); });
    expRow->addWidget(m_exportFrdBtn);

    m_exportCalFrdBtn = new QPushButton("Calibrated", audioBox);
    connect(m_exportCalFrdBtn, &QPushButton::clicked, [this]() { onExportFRD(true); });
    m_exportCalFrdBtn->setEnabled(false);
    expRow->addWidget(m_exportCalFrdBtn);
    audioLayout->addLayout(expRow);

    layout->addWidget(createSidebarSection("Audio Setup", audioBox));

    // 2. Target & Analysis Section
    auto targetBox = new QWidget(content);
    auto targetForm = new QFormLayout(targetBox);
    targetForm->setContentsMargins(0, 0, 0, 0);

    m_targetPresetCombo = new QComboBox(targetBox);
    m_targetPresetCombo->addItems({"Flat (0 dB)", "Brüel & Kjær", "Harman"});
    connect(m_targetPresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.customTarget = std::nullopt;
        m_session.targetPreset = static_cast<TargetPreset>(idx);
        m_session.recomputeAverage();
    });
    targetForm->addRow("Target Curve:", m_targetPresetCombo);

    m_smoothingCombo = new QComboBox(targetBox);
    m_smoothingCombo->addItems({"Off", "1/3 oct", "1/6 oct", "1/12 oct", "1/24 oct"});
    m_smoothingCombo->setCurrentIndex(2); // 1/6 oct
    connect(m_smoothingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        switch (idx) {
        case 0:
            m_session.displaySmoothing = DisplaySmoothing::Off;
            break;
        case 1:
            m_session.displaySmoothing = DisplaySmoothing::Oct1over3;
            break;
        case 2:
            m_session.displaySmoothing = DisplaySmoothing::Oct1over6;
            break;
        case 3:
            m_session.displaySmoothing = DisplaySmoothing::Oct1over12;
            break;
        case 4:
            m_session.displaySmoothing = DisplaySmoothing::Oct1over24;
            break;
        }
        refreshSessionUi();
    });
    targetForm->addRow("Display Smoothing:", m_smoothingCombo);

    m_fdwCombo = new QComboBox(targetBox);
    m_fdwCombo->addItems({"Off", "1 cycle", "5 cycles", "10 cycles", "15 cycles"});
    connect(m_fdwCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        switch (idx) {
        case 0:
            m_session.fdwCycles = FDWCycles::Off;
            break;
        case 1:
            m_session.fdwCycles = FDWCycles::Cycles1;
            break;
        case 2:
            m_session.fdwCycles = FDWCycles::Cycles5;
            break;
        case 3:
            m_session.fdwCycles = FDWCycles::Cycles10;
            break;
        case 4:
            m_session.fdwCycles = FDWCycles::Cycles15;
            break;
        }
        m_session.recomputeAverage();
    });
    targetForm->addRow("FDW (Cycles):", m_fdwCombo);

    layout->addWidget(createSidebarSection("Target & Analysis", targetBox));

    // 3. Modal Region Section
    auto modalBox = new QWidget(content);
    auto modalLayout = new QVBoxLayout(modalBox);
    modalLayout->setContentsMargins(0, 0, 0, 0);
    modalLayout->setSpacing(6);

    m_modalModeCheck = new QCheckBox("Apply Constraints", modalBox);
    connect(m_modalModeCheck, &QCheckBox::toggled, [this](bool checked) {
        m_session.modalMode = checked;
        m_schroederCombo->setEnabled(checked);
        m_modalMinQCombo->setEnabled(checked);
    });
    modalLayout->addWidget(m_modalModeCheck);

    auto modalForm = new QFormLayout();
    m_schroederCombo = new QComboBox(modalBox);
    m_schroederCombo->addItems({"100 Hz", "150 Hz", "200 Hz", "250 Hz", "300 Hz", "400 Hz"});
    m_schroederCombo->setCurrentIndex(2); // 200 Hz
    connect(m_schroederCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        const double freqs[] = {100.0, 150.0, 200.0, 250.0, 300.0, 400.0};
        m_session.schroederHz = freqs[idx];
    });
    modalForm->addRow("Schroeder Freq:", m_schroederCombo);

    m_modalMinQCombo = new QComboBox(modalBox);
    m_modalMinQCombo->addItems({"1.5", "2.0", "2.5", "3.0", "4.0"});
    m_modalMinQCombo->setCurrentIndex(1); // 2.0
    connect(m_modalMinQCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        const double qs[] = {1.5, 2.0, 2.5, 3.0, 4.0};
        m_session.modalMinQ = qs[idx];
    });
    modalForm->addRow("Minimum Q Limit:", m_modalMinQCombo);

    m_schroederCombo->setEnabled(false);
    m_modalMinQCombo->setEnabled(false);

    modalLayout->addLayout(modalForm);
    layout->addWidget(createSidebarSection("Modal Region", modalBox));

    // 4. PEQ Design Section
    auto peqBox = new QWidget(content);
    auto peqLayout = new QVBoxLayout(peqBox);
    peqLayout->setContentsMargins(0, 0, 0, 0);
    peqLayout->setSpacing(6);

    auto peqForm = new QFormLayout();
    m_bandCountCombo = new QComboBox(peqBox);
    m_bandCountCombo->addItems({"3 bands", "5 bands", "8 bands", "10 bands", "12 bands", "16 bands", "20 bands"});
    m_bandCountCombo->setCurrentIndex(2); // 8 bands
    connect(m_bandCountCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        const int counts[] = {3, 5, 8, 10, 12, 16, 20};
        m_session.bandCount = counts[idx];
    });
    peqForm->addRow("Bands Limit:", m_bandCountCombo);
    peqLayout->addLayout(peqForm);

    m_generatePeqBtn = new QPushButton("✨ Generate PEQ", peqBox);
    m_generatePeqBtn->setStyleSheet(
        "QPushButton { background-color: #007acc; color: white; font-weight: bold; padding: 6px; }");
    connect(m_generatePeqBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onRunFit);
    peqLayout->addWidget(m_generatePeqBtn);

    m_addToEqPresetsBtn = new QPushButton("➕ Add to EQ Presets", peqBox);
    connect(m_addToEqPresetsBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onApplyEQToPipeline);
    peqLayout->addWidget(m_addToEqPresetsBtn);

    layout->addWidget(createSidebarSection("Parametric EQ (PEQ)", peqBox));

    // 5. FIR Convolution Design Section
    auto firBox = new QWidget(content);
    auto firLayout = new QVBoxLayout(firBox);
    firLayout->setContentsMargins(0, 0, 0, 0);
    firLayout->setSpacing(6);

    auto firForm = new QFormLayout();
    m_firKindCombo = new QComboBox(firBox);
    m_firKindCombo->addItems({"Min-phase", "Linear-phase", "From measurement"});
    connect(m_firKindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.firKind = static_cast<FIRKind>(idx);
        m_phaseBlendContainer->setVisible(idx == 2);
    });
    firForm->addRow("Filter Type:", m_firKindCombo);

    m_firTapCombo = new QComboBox(firBox);
    m_firTapCombo->addItems({"2048", "4096", "8192", "16384", "32768"});
    m_firTapCombo->setCurrentIndex(2); // 8192
    connect(m_firTapCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        const int taps[] = {2048, 4096, 8192, 16384, 32768};
        m_session.firTapCount = taps[idx];
    });
    firForm->addRow("Tap Count Length:", m_firTapCombo);
    firLayout->addLayout(firForm);

    m_phaseBlendContainer = new QWidget(firBox);
    auto blendLayout = new QVBoxLayout(m_phaseBlendContainer);
    blendLayout->setContentsMargins(0, 0, 0, 0);
    blendLayout->setSpacing(2);

    auto blendHeader = new QHBoxLayout();
    blendHeader->addWidget(new QLabel("Phase Blend", m_phaseBlendContainer));
    m_phaseBlendValueLabel = new QLabel("Linear-phase", m_phaseBlendContainer);
    m_phaseBlendValueLabel->setStyleSheet("font-weight: bold; font-size: 11px;");
    blendHeader->addWidget(m_phaseBlendValueLabel, 0, Qt::AlignRight);
    blendLayout->addLayout(blendHeader);

    m_phaseBlendSlider = new QSlider(Qt::Horizontal, m_phaseBlendContainer);
    m_phaseBlendSlider->setRange(0, 100);
    m_phaseBlendSlider->setValue(100);
    connect(m_phaseBlendSlider, &QSlider::valueChanged, [this](int val) {
        double blend = val / 100.0;
        m_session.firPhaseBlend = blend;
        if (val <= 1)
            m_phaseBlendValueLabel->setText("Min-phase");
        else if (val >= 99)
            m_phaseBlendValueLabel->setText("Linear-phase");
        else
            m_phaseBlendValueLabel->setText(QString("%1%").arg(val));
    });
    blendLayout->addWidget(m_phaseBlendSlider);

    m_phaseBlendContainer->setVisible(false);
    firLayout->addWidget(m_phaseBlendContainer);

    m_addToFirPresetsBtn = new QPushButton("➕ Add to FIR Presets", firBox);
    connect(m_addToFirPresetsBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onGenerateFIR);
    firLayout->addWidget(m_addToFirPresetsBtn);

    layout->addWidget(createSidebarSection("FIR Convolution", firBox));

    layout->addStretch(1);
    scroll->setWidget(content);
    return scroll;
}

QWidget* RoomCorrectionDlg::createSidebarSection(const QString& title, QWidget* content) {
    auto group = new QGroupBox(title, this);
    group->setStyleSheet("QGroupBox { font-weight: bold; font-size: 12px; margin-top: 6px; } QGroupBox::title { "
                         "subcontrol-origin: margin; left: 0px; }");
    auto layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 12, 0, 4);
    layout->addWidget(content);
    return group;
}

void RoomCorrectionDlg::toggleSidebar() {
    m_sidebarVisible = !m_sidebarVisible;
    m_sidebarWidget->setVisible(m_sidebarVisible);
}

void RoomCorrectionDlg::populateAudioDevices() {
#ifdef QT_MULTIMEDIA_LIB
    m_micDeviceCombo->clear();
    m_micDeviceCombo->addItem("System Default", "");
    for (const auto& dev : QMediaDevices::audioInputs()) {
        m_micDeviceCombo->addItem(dev.description(), dev.description());
    }

    m_outputDeviceCombo->clear();
    m_outputDeviceCombo->addItem("System Default", "");
    for (const auto& dev : QMediaDevices::audioOutputs()) {
        m_outputDeviceCombo->addItem(dev.description(), dev.description());
    }
#endif
    updateMicChannels();
    updateOutputChannels();
}

void RoomCorrectionDlg::updateMicChannels() {
    m_micChannelCombo->clear();
    m_micChannelCombo->addItem("Channel 1", 0);
    m_micChannelCombo->addItem("Channel 2", 1);
}

void RoomCorrectionDlg::updateOutputChannels() {
    m_outputChannelCombo->clear();
    m_outputChannelCombo->addItem("All channels", -1);
    m_outputChannelCombo->addItem("Channel 1 (Left)", 0);
    m_outputChannelCombo->addItem("Channel 2 (Right)", 1);
    m_outputChannelCombo->addItem("Channel 4 (LFE)", 3);
}

void RoomCorrectionDlg::refreshSessionUi() {
    m_statusLabel->setText(QString::fromStdString(m_session.status));

    // Clear chips layout
    QLayoutItem* item;
    while ((item = m_positionsChipsLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    for (const auto& p : m_session.positions) {
        auto chip = new MeasurementPositionRowWidget(p, &m_session, m_positionsContainer);
        connect(chip, &MeasurementPositionRowWidget::positionChanged, this, &RoomCorrectionDlg::refreshSessionUi);
        m_positionsChipsLayout->addWidget(chip);
    }
    m_positionsChipsLayout->addStretch(1);

    // Subwoofer Assist button visibility
    m_subwooferAssistBtn->setVisible(m_session.subwooferAssistAvailable());

    // Calibration state
    if (m_session.calibrationPath.empty()) {
        m_calPathLabel->setText("None loaded");
        m_clearCalBtn->setVisible(false);
        m_exportCalFrdBtn->setEnabled(false);
    } else {
        QFileInfo fi(QString::fromStdString(m_session.calibrationPath));
        m_calPathLabel->setText(fi.fileName());
        m_clearCalBtn->setVisible(true);
        m_exportCalFrdBtn->setEnabled(m_session.measuredFR.has_value());
    }

    m_exportFrdBtn->setEnabled(m_session.measuredFR.has_value());

    // Refresh Plots
    if (m_session.correctionPreset.has_value()) {
        m_frDiagramWidget->setPreset(m_session.correctionPreset.value());
    }

    EQReferenceOverlayData overlay;
    overlay.frequencies = m_session.grid;
    overlay.measuredMagDB = m_session.displayedMagDB();
    overlay.targetCurve = m_session.targetCurve();
    overlay.showCorrected = true;
    overlay.active = !overlay.measuredMagDB.empty() && m_session.correctionPreset.has_value();
    m_frDiagramWidget->setReferenceOverlay(overlay);

    if (m_session.measuredIR.has_value()) {
        m_waterfallWidget->recomputeSTFTAsync(m_session.measuredIR.value());
    }

    m_phasePlotWidget->update();
    m_impulsePlotWidget->update();
    m_groupDelayPlotWidget->update();
}

void RoomCorrectionDlg::onGenerateMock(bool append) {
    m_session.generateMockMeasurement(append);
    refreshSessionUi();
}

void RoomCorrectionDlg::onRecordHardwareMeasurement(bool append) {
    m_session.recordPosition(append, m_session.selectedMicName, m_session.selectedOutputName,
                             m_session.selectedInputChannel, m_session.selectedOutputChannel,
                             [this](bool success, const std::string& msg) {
                                 (void)success;
                                 m_statusLabel->setText(QString::fromStdString(msg));
                                 refreshSessionUi();
                             });
}

void RoomCorrectionDlg::onImportFRD() {
    QString path = QFileDialog::getOpenFileName(this, "Import FRD as Position", "", "FRD Files (*.frd *.txt)");
    if (!path.isEmpty()) {
        m_session.importPositionFRD(path.toStdString());
        refreshSessionUi();
    }
}

void RoomCorrectionDlg::onExportFRD(bool includeCalibration) {
    QString suffix = includeCalibration ? "-calibrated" : "";
    QString defaultName = QString("Measurement-%1Hz%2.frd").arg(m_session.sampleRate).arg(suffix);
    QString path = QFileDialog::getSaveFileName(this, "Export REW FRD", defaultName, "FRD Files (*.frd *.txt)");
    if (!path.isEmpty()) {
        m_session.exportFRD(path.toStdString(), includeCalibration);
        refreshSessionUi();
    }
}

void RoomCorrectionDlg::onLoadCalibration() {
    QString path = QFileDialog::getOpenFileName(this, "Load Microphone Calibration File", "",
                                                "Calibration Files (*.frd *.txt *.cal)");
    if (!path.isEmpty()) {
        m_session.loadCalibration(path.toStdString());
        refreshSessionUi();
    }
}

void RoomCorrectionDlg::onClearCalibration() {
    m_session.clearCalibration();
    refreshSessionUi();
}

void RoomCorrectionDlg::onRunFit() {
    m_session.runFit();
    refreshSessionUi();
}

void RoomCorrectionDlg::onApplyEQToPipeline() {
    if (m_session.correctionPreset.has_value()) {
        const auto& preset = m_session.correctionPreset.value();
        m_pipeline->addEQPreset(preset);
        m_session.status = "Applied as EQ Preset “" + preset.name + ".” Open it from the sidebar to edit.";
        refreshSessionUi();
        QMessageBox::information(this, "Applied", "Fitted PEQ preset applied to pipeline!");
    }
}

void RoomCorrectionDlg::onGenerateFIR() {
    std::vector<std::string> names;
    for (const auto& p : m_pipeline->convPresets)
        names.push_back(p.name);

    auto preset = m_session.generateFIR(names);
    if (preset.has_value()) {
        m_pipeline->addConvPreset(preset.value());
        refreshSessionUi();
        QMessageBox::information(this, "Success", "FIR Preset generated and added to Convolution presets!");
    }
}

void RoomCorrectionDlg::onComputeSubwoofer() {
    auto rec = m_session.computeSubwooferRecommendation();
    if (rec.has_value()) {
        QString text = QString("Subwoofer Recommendation:\n\n"
                               "• Sub Delay Offset: %1 ms (%2 samples)\n"
                               "• Recommended Crossover: %3 Hz\n"
                               "• Confidence: %4%\n\n"
                               "Rationale:\n%5")
                           .arg(rec->subDelayMs, 0, 'f', 2)
                           .arg(rec->delaySamples)
                           .arg(rec->crossoverHz, 0, 'f', 0)
                           .arg(rec->confidence * 100.0, 0, 'f', 0)
                           .arg(QString::fromStdString(rec->summary));

        QMessageBox::information(this, "Subwoofer Crossover Assist", text);
    } else {
        QMessageBox::warning(
            this, "Subwoofer Assist",
            "Could not compute recommendation. Ensure one Mains and one Subwoofer position are loaded.");
    }
}
