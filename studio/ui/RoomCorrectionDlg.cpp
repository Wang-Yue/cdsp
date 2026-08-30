#include "ui/RoomCorrectionDlg.h"

#include "models/ConvolutionPreset.h"          // for ConvolutionPreset
#include "models/EQPreset.h"                   // for EQPreset, EQBand
#include "room_correction/FrequencyResponse.h" // for FrequencyResponse
#include "room_correction/ImpulseResponse.h"   // for ImpulseResponse
#include "room_correction/TargetCurve.h"       // for TargetCurve, TargetPreset
#include "ui/MeasurementPositionRowWidget.h"   // for MeasurementPositionRowWidget
#include "ui/SubwooferAssistDlg.h"             // for SubwooferAssistDlg

#include <QDialogButtonBox> // for QDialogButtonBox
#include <QFileDialog>      // for QFileDialog
#include <QFileInfo>        // for QFileInfo
#include <QFormLayout>      // for QFormLayout
#include <QFrame>           // for QFrame
#include <QGroupBox>        // for QGroupBox
#include <QLayoutItem>      // for QLayoutItem
#include <QList>            // for QList
#include <QMenu>            // for QMenu
#include <QMessageBox>      // for QMessageBox
#include <QScrollArea>      // for QScrollArea
#include <QSizePolicy>      // for QSizePolicy
#include <QSplitter>        // for QSplitter
#include <QStatusBar>       // for QStatusBar
#include <QString>          // for QString, operator==
#include <QToolBar>         // for QToolBar
#include <QVBoxLayout>      // for QVBoxLayout
#include <QVariant>         // for QVariant
#include <Qt>               // for ScrollBarPolicy, Orientation, AlignmentFlag
#include <QtGlobal>         // for QOverload
#include <algorithm>        // for max
#include <functional>       // for function
#include <optional>         // for optional, nullopt, nullopt_t
#include <string>           // for basic_string, operator+, string
#include <vector>           // for vector

#ifdef QT_MULTIMEDIA_LIB
#include <QAudioDevice>  // for QAudioDevice
#include <QMediaDevices> // for QMediaDevices
#endif

RoomCorrectionDlg::RoomCorrectionDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent)
    : QDialog(parent), m_pipeline(pipeline) {
    setWindowTitle("Room Correction Studio");
    resize(1150, 760);
    setMinimumSize(960, 680);
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

    // Main Content Area (Plots + Positions Bar + Sidebar in QSplitter)
    mainLayout->addWidget(createMainArea(), 1);

    // Bottom Status Bar
    auto statusBar = new QStatusBar(this);
    m_statusLabel = new QLabel("Ready.", statusBar);
    statusBar->addWidget(m_statusLabel, 1);

    auto buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, statusBar);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    statusBar->addPermanentWidget(buttonBox);

    mainLayout->addWidget(statusBar);

    populateAudioDevices();
}

QWidget* RoomCorrectionDlg::createHeaderToolbar() {
    auto toolbar = new QToolBar("Room Correction Toolbar", this);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);

    // Measure Dropdown Menu Button
    m_measureMenuBtn = new QPushButton("Measure ▾", toolbar);
    auto measureMenu = new QMenu(m_measureMenuBtn);

    measureMenu->addSection("Real measurement");
    m_newCapAction = measureMenu->addAction("🎤 New Capture");
    connect(m_newCapAction, &QAction::triggered, [this]() { onRecordHardwareMeasurement(false); });

    m_addCapAction = measureMenu->addAction("➕ Add Capture as Position");
    connect(m_addCapAction, &QAction::triggered, [this]() { onRecordHardwareMeasurement(true); });

    measureMenu->addSection("Mock");
    m_newMockAction = measureMenu->addAction("🎲 New Mock Measurement");
    connect(m_newMockAction, &QAction::triggered, [this]() { onGenerateMock(false); });

    m_addMockAction = measureMenu->addAction("➕ Add Mock Position");
    connect(m_addMockAction, &QAction::triggered, [this]() { onGenerateMock(true); });

    measureMenu->addSection("Import");
    m_importFrdAction = measureMenu->addAction("📥 Import FRD as Position…");
    connect(m_importFrdAction, &QAction::triggered, this, &RoomCorrectionDlg::onImportFRD);

    m_measureMenuBtn->setMenu(measureMenu);
    toolbar->addWidget(m_measureMenuBtn);

    auto leftSpacer = new QWidget(toolbar);
    leftSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(leftSpacer);

    // Pane Picker Segmented TabBar
    m_paneTabBar = new QTabBar(toolbar);
    m_paneTabBar->setExpanding(false);
    m_paneTabBar->addTab("Magnitude");
    m_paneTabBar->addTab("Phase");
    m_paneTabBar->addTab("Impulse");
    m_paneTabBar->addTab("Group Delay");
    m_paneTabBar->addTab("Waterfall (CSD)");
    connect(m_paneTabBar, &QTabBar::currentChanged, [this](int idx) { m_plotStackedWidget->setCurrentIndex(idx); });
    toolbar->addWidget(m_paneTabBar);

    auto rightSpacer = new QWidget(toolbar);
    rightSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(rightSpacer);

    // Sidebar Toggle Button
    m_sidebarToggleBtn = new QToolButton(toolbar);
    m_sidebarToggleBtn->setText("▤");
    m_sidebarToggleBtn->setToolTip("Toggle Sidebar");
    connect(m_sidebarToggleBtn, &QToolButton::clicked, this, &RoomCorrectionDlg::toggleSidebar);
    toolbar->addWidget(m_sidebarToggleBtn);

    return toolbar;
}

QWidget* RoomCorrectionDlg::createMainArea() {
    auto splitter = new QSplitter(Qt::Horizontal, this);

    // Left Panel (Plot Stacked Widget + Positions Bar)
    auto leftWidget = new QWidget(splitter);
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

    // Positions Bar
    m_positionsContainer = new QWidget(leftWidget);
    auto posBarLayout = new QHBoxLayout(m_positionsContainer);
    posBarLayout->setContentsMargins(8, 4, 8, 4);
    posBarLayout->setSpacing(8);

    auto posLabel = new QLabel("Positions", m_positionsContainer);
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
    connect(m_subwooferAssistBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onComputeSubwoofer);
    m_subwooferAssistBtn->setVisible(false);
    posBarLayout->addWidget(m_subwooferAssistBtn);

    leftLayout->addWidget(m_positionsContainer);

    splitter->addWidget(leftWidget);

    // Right Sidebar
    m_sidebarWidget = createSidebar();
    splitter->addWidget(m_sidebarWidget);

    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({800, 320});

    return splitter;
}

QWidget* RoomCorrectionDlg::createSidebar() {
    auto scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto content = new QWidget(scroll);
    auto layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(10);

    // 1. Microphone Input GroupBox
    auto micGroup = new QGroupBox("Microphone Input", content);
    auto micForm = new QFormLayout(micGroup);

    m_micDeviceCombo = new QComboBox(micGroup);
    m_micDeviceCombo->addItem("System Default", "");
    connect(m_micDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.selectedMicName = m_micDeviceCombo->itemData(idx).toString().toStdString();
        updateMicChannels();
    });
    micForm->addRow("Device:", m_micDeviceCombo);

    m_micChannelCombo = new QComboBox(micGroup);
    m_micChannelCombo->addItem("Channel 1", 0);
    connect(m_micChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int idx) { m_session.selectedInputChannel = m_micChannelCombo->itemData(idx).toInt(); });
    micForm->addRow("Channel:", m_micChannelCombo);

    layout->addWidget(micGroup);

    // 2. Speaker Output GroupBox
    auto outputGroup = new QGroupBox("Speaker Output", content);
    auto outputForm = new QFormLayout(outputGroup);

    m_outputDeviceCombo = new QComboBox(outputGroup);
    m_outputDeviceCombo->addItem("System Default", "");
    connect(m_outputDeviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.selectedOutputName = m_outputDeviceCombo->itemData(idx).toString().toStdString();
        updateOutputChannels();
    });
    outputForm->addRow("Device:", m_outputDeviceCombo);

    m_outputChannelCombo = new QComboBox(outputGroup);
    m_outputChannelCombo->addItem("All channels", -1);
    connect(m_outputChannelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int idx) { m_session.selectedOutputChannel = m_outputChannelCombo->itemData(idx).toInt(); });
    outputForm->addRow("Channel:", m_outputChannelCombo);

    layout->addWidget(outputGroup);

    // 3. Calibration Curve GroupBox
    auto calGroup = new QGroupBox("Calibration Curve", content);
    auto calForm = new QFormLayout(calGroup);

    auto calRowWidget = new QWidget(calGroup);
    auto calRow = new QHBoxLayout(calRowWidget);
    calRow->setContentsMargins(0, 0, 0, 0);
    m_calPathLabel = new QLabel("None loaded", calRowWidget);
    calRow->addWidget(m_calPathLabel, 1);

    m_loadCalBtn = new QPushButton("Load…", calRowWidget);
    connect(m_loadCalBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onLoadCalibration);
    calRow->addWidget(m_loadCalBtn);

    m_clearCalBtn = new QToolButton(calRowWidget);
    m_clearCalBtn->setText("✕");
    m_clearCalBtn->setVisible(false);
    connect(m_clearCalBtn, &QToolButton::clicked, this, &RoomCorrectionDlg::onClearCalibration);
    calRow->addWidget(m_clearCalBtn);
    calForm->addRow("File:", calRowWidget);

    auto expRowWidget = new QWidget(calGroup);
    auto expRow = new QHBoxLayout(expRowWidget);
    expRow->setContentsMargins(0, 0, 0, 0);
    m_exportFrdBtn = new QPushButton("Export FRD", expRowWidget);
    connect(m_exportFrdBtn, &QPushButton::clicked, [this]() { onExportFRD(false); });
    expRow->addWidget(m_exportFrdBtn);

    m_exportCalFrdBtn = new QPushButton("Calibrated", expRowWidget);
    connect(m_exportCalFrdBtn, &QPushButton::clicked, [this]() { onExportFRD(true); });
    m_exportCalFrdBtn->setEnabled(false);
    expRow->addWidget(m_exportCalFrdBtn);
    calForm->addRow("Export:", expRowWidget);

    layout->addWidget(calGroup);

    // 4. Measurement Parameters GroupBox
    auto sweepGroup = new QGroupBox("Measurement Parameters", content);
    auto sweepForm = new QFormLayout(sweepGroup);

    m_sweepF1Spin = new QDoubleSpinBox(sweepGroup);
    m_sweepF1Spin->setRange(10.0, 1000.0);
    m_sweepF1Spin->setValue(m_session.sweepF1);
    m_sweepF1Spin->setSuffix(" Hz");
    connect(m_sweepF1Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [this](double val) { m_session.sweepF1 = val; });
    sweepForm->addRow("Start Freq:", m_sweepF1Spin);

    m_sweepF2Spin = new QDoubleSpinBox(sweepGroup);
    m_sweepF2Spin->setRange(1000.0, 96000.0);
    m_sweepF2Spin->setValue(m_session.sweepF2);
    m_sweepF2Spin->setSuffix(" Hz");
    connect(m_sweepF2Spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [this](double val) { m_session.sweepF2 = val; });
    sweepForm->addRow("End Freq:", m_sweepF2Spin);

    m_sweepDurationSpin = new QDoubleSpinBox(sweepGroup);
    m_sweepDurationSpin->setRange(0.1, 30.0);
    m_sweepDurationSpin->setSingleStep(0.5);
    m_sweepDurationSpin->setValue(m_session.sweepDurationSeconds);
    m_sweepDurationSpin->setSuffix(" s");
    connect(m_sweepDurationSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [this](double val) { m_session.sweepDurationSeconds = val; });
    sweepForm->addRow("Duration:", m_sweepDurationSpin);

    layout->addWidget(sweepGroup);

    // 5. Target Curve GroupBox
    auto targetGroup = new QGroupBox("Target Curve", content);
    auto targetForm = new QFormLayout(targetGroup);

    m_targetPresetCombo = new QComboBox(targetGroup);
    m_targetPresetCombo->addItems({"Flat (0 dB)", "Brüel & Kjær", "Harman", "Tilt (-1 dB/oct)", "Bass Boost (+4 dB)"});
    m_targetPresetCombo->setCurrentIndex(static_cast<int>(m_session.targetPreset));
    connect(m_targetPresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.customTarget = std::nullopt;
        m_session.targetPreset = static_cast<TargetPreset>(idx);
        m_session.recomputeAverage();
    });
    targetForm->addRow("Target:", m_targetPresetCombo);

    m_smoothingCombo = new QComboBox(targetGroup);
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
    targetForm->addRow("Smoothing:", m_smoothingCombo);

    m_fdwCombo = new QComboBox(targetGroup);
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

    layout->addWidget(targetGroup);

    // 6. Modal Region GroupBox
    auto modalGroup = new QGroupBox("Modal Region", content);
    auto modalLayout = new QVBoxLayout(modalGroup);

    m_modalModeCheck = new QCheckBox("Apply Constraints", modalGroup);
    connect(m_modalModeCheck, &QCheckBox::toggled, [this](bool checked) {
        m_session.modalMode = checked;
        m_modalParamsContainer->setVisible(checked);
    });
    modalLayout->addWidget(m_modalModeCheck);

    m_modalParamsContainer = new QWidget(modalGroup);
    auto modalForm = new QFormLayout(m_modalParamsContainer);
    modalForm->setContentsMargins(0, 0, 0, 0);
    m_schroederCombo = new QComboBox(m_modalParamsContainer);
    m_schroederCombo->addItems({"100 Hz", "150 Hz", "200 Hz", "250 Hz", "300 Hz", "400 Hz"});
    m_schroederCombo->setCurrentIndex(2); // 200 Hz
    connect(m_schroederCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        const double freqs[] = {100.0, 150.0, 200.0, 250.0, 300.0, 400.0};
        m_session.schroederHz = freqs[idx];
    });
    modalForm->addRow("Schroeder:", m_schroederCombo);

    m_modalMinQCombo = new QComboBox(m_modalParamsContainer);
    m_modalMinQCombo->addItems({"1.5", "2.0", "2.5", "3.0", "4.0"});
    m_modalMinQCombo->setCurrentIndex(1); // 2.0
    connect(m_modalMinQCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        const double qs[] = {1.5, 2.0, 2.5, 3.0, 4.0};
        m_session.modalMinQ = qs[idx];
    });
    modalForm->addRow("Min Q Limit:", m_modalMinQCombo);

    m_modalParamsContainer->setVisible(false);
    modalLayout->addWidget(m_modalParamsContainer);

    layout->addWidget(modalGroup);

    // 7. Target EQ Parameters GroupBox
    auto peqGroup = new QGroupBox("Target EQ Parameters", content);
    auto peqLayout = new QVBoxLayout(peqGroup);

    auto peqForm = new QFormLayout();
    m_bandCountCombo = new QComboBox(peqGroup);
    m_bandCountCombo->addItems({"3 bands", "5 bands", "8 bands", "10 bands", "12 bands", "16 bands", "20 bands"});
    m_bandCountCombo->setCurrentIndex(2); // 8 bands
    connect(m_bandCountCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        const int counts[] = {3, 5, 8, 10, 12, 16, 20};
        m_session.bandCount = counts[idx];
    });
    peqForm->addRow("Bands Limit:", m_bandCountCombo);
    peqLayout->addLayout(peqForm);

    m_generatePeqBtn = new QPushButton("✨ Generate PEQ", peqGroup);
    connect(m_generatePeqBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onRunFit);
    peqLayout->addWidget(m_generatePeqBtn);

    m_addToEqPresetsBtn = new QPushButton("➕ Add to EQ Presets", peqGroup);
    connect(m_addToEqPresetsBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onApplyEQToPipeline);
    peqLayout->addWidget(m_addToEqPresetsBtn);

    layout->addWidget(peqGroup);

    // 8. FIR Convolution GroupBox
    auto firGroup = new QGroupBox("FIR Convolution", content);
    auto firLayout = new QVBoxLayout(firGroup);

    auto firForm = new QFormLayout();
    m_firKindCombo = new QComboBox(firGroup);
    m_firKindCombo->addItems({"Min-phase", "Linear-phase", "From measurement"});
    connect(m_firKindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        m_session.firKind = static_cast<FIRKind>(idx);
        m_phaseBlendContainer->setVisible(idx == 2);
        refreshSessionUi();
    });
    firForm->addRow("Filter Type:", m_firKindCombo);

    m_firTapCombo = new QComboBox(firGroup);
    m_firTapCombo->addItems({"2 048", "4 096", "8 192", "16 384", "32 768"});
    m_firTapCombo->setCurrentIndex(2); // 8192
    connect(m_firTapCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int idx) {
        const int taps[] = {2048, 4096, 8192, 16384, 32768};
        m_session.firTapCount = taps[idx];
    });
    firForm->addRow("Tap Count:", m_firTapCombo);
    firLayout->addLayout(firForm);

    m_phaseBlendContainer = new QWidget(firGroup);
    auto blendLayout = new QVBoxLayout(m_phaseBlendContainer);
    blendLayout->setContentsMargins(0, 0, 0, 0);
    blendLayout->setSpacing(2);

    auto blendHeader = new QHBoxLayout();
    blendHeader->addWidget(new QLabel("Phase Blend", m_phaseBlendContainer));
    m_phaseBlendValueLabel = new QLabel("Linear-phase", m_phaseBlendContainer);
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

    m_addToFirPresetsBtn = new QPushButton("➕ Add to FIR Presets", firGroup);
    connect(m_addToFirPresetsBtn, &QPushButton::clicked, this, &RoomCorrectionDlg::onGenerateFIR);
    firLayout->addWidget(m_addToFirPresetsBtn);

    layout->addWidget(firGroup);

    layout->addStretch(1);
    scroll->setWidget(content);
    return scroll;
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
    int channels = 2; // Default fallback
#ifdef QT_MULTIMEDIA_LIB
    QString selectedName = m_micDeviceCombo->currentData().toString();
    if (!selectedName.isEmpty()) {
        for (const auto& dev : QMediaDevices::audioInputs()) {
            if (dev.description() == selectedName) {
                channels = dev.maximumChannelCount();
                break;
            }
        }
    } else {
        auto dev = QMediaDevices::defaultAudioInput();
        if (!dev.isNull()) {
            channels = dev.maximumChannelCount();
        }
    }
#endif
    for (int i = 0; i < std::max(1, channels); ++i) {
        m_micChannelCombo->addItem(QString("Channel %1").arg(i + 1), i);
    }
}

void RoomCorrectionDlg::updateOutputChannels() {
    m_outputChannelCombo->clear();
    m_outputChannelCombo->addItem("All channels", -1);
    int channels = 2; // Default fallback
#ifdef QT_MULTIMEDIA_LIB
    QString selectedName = m_outputDeviceCombo->currentData().toString();
    if (!selectedName.isEmpty()) {
        for (const auto& dev : QMediaDevices::audioOutputs()) {
            if (dev.description() == selectedName) {
                channels = dev.maximumChannelCount();
                break;
            }
        }
    } else {
        auto dev = QMediaDevices::defaultAudioOutput();
        if (!dev.isNull()) {
            channels = dev.maximumChannelCount();
        }
    }
#endif
    for (int i = 0; i < std::max(1, channels); ++i) {
        QString label;
        if (channels == 2) {
            if (i == 0)
                label = "Channel 1 (Left)";
            else if (i == 1)
                label = "Channel 2 (Right)";
        } else if (channels >= 6 && i == 3) {
            label = "Channel 4 (LFE)";
        } else {
            label = QString("Channel %1").arg(i + 1);
        }
        m_outputChannelCombo->addItem(label, i);
    }
}

void RoomCorrectionDlg::refreshSessionUi() {
    m_statusLabel->setText(QString::fromStdString(m_session.status));

    bool isCap = m_session.isCapturing;
    bool hasPos = !m_session.positions.empty();

    if (isCap) {
        m_measureMenuBtn->setText("Capturing… ▾");
        m_measureMenuBtn->setEnabled(false);
    } else {
        m_measureMenuBtn->setText("Measure ▾");
        m_measureMenuBtn->setEnabled(true);
    }

    if (m_newCapAction)
        m_newCapAction->setEnabled(!isCap);
    if (m_addCapAction)
        m_addCapAction->setEnabled(!isCap && hasPos);
    if (m_newMockAction)
        m_newMockAction->setEnabled(true);
    if (m_addMockAction)
        m_addMockAction->setEnabled(hasPos);

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
        m_exportCalFrdBtn->setVisible(false);
        m_exportCalFrdBtn->setEnabled(false);
    } else {
        QFileInfo fi(QString::fromStdString(m_session.calibrationPath));
        m_calPathLabel->setText(fi.fileName());
        m_clearCalBtn->setVisible(true);
        m_exportCalFrdBtn->setVisible(true);
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

    // Button states
    bool hasBands = m_session.correctionPreset.has_value() && !m_session.correctionPreset->bands.empty();
    m_generatePeqBtn->setEnabled(!m_session.measuredMagDB.empty());
    m_addToEqPresetsBtn->setEnabled(hasBands);

    bool canGenerateFIR =
        (m_session.firKind == FIRKind::MeasurementDriven) ? m_session.measuredFR.has_value() : hasBands;
    m_addToFirPresetsBtn->setEnabled(canGenerateFIR);
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
    SubwooferAssistDlg dlg(&m_session, m_pipeline, this);
    dlg.exec();
}
