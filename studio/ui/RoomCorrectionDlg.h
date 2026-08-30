#ifndef ROOM_CORRECTION_DLG_H
#define ROOM_CORRECTION_DLG_H

#include "models/PipelineStore.h"               // for PipelineStore
#include "room_correction/MeasurementSession.h" // for MeasurementSession
#include "ui/EQDiagramWidget.h"                 // for EQDiagramWidget
#include "ui/GroupDelayPlotWidget.h"            // for GroupDelayPlotWidget
#include "ui/ImpulseResponsePlotWidget.h"       // for ImpulseResponsePlotWidget
#include "ui/PhasePlotWidget.h"                 // for PhasePlotWidget
#include "ui/WaterfallPlotWidget.h"             // for WaterfallPlotWidget

#include <QAction>        // for QAction
#include <QCheckBox>      // for QCheckBox
#include <QComboBox>      // for QComboBox
#include <QDialog>        // for QDialog
#include <QDoubleSpinBox> // for QDoubleSpinBox
#include <QHBoxLayout>    // for QHBoxLayout
#include <QLabel>         // for QLabel
#include <QObject>        // for Q_OBJECT, slots
#include <QPushButton>    // for QPushButton
#include <QSlider>        // for QSlider
#include <QStackedWidget> // for QStackedWidget
#include <QTabBar>        // for QTabBar
#include <QToolButton>    // for QToolButton
#include <QWidget>        // for QWidget
#include <memory>         // for shared_ptr

class RoomCorrectionDlg : public QDialog {
    Q_OBJECT

public:
    RoomCorrectionDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr);

private slots:
    void refreshSessionUi();
    void onGenerateMock(bool append);
    void onRecordHardwareMeasurement(bool append);
    void onImportFRD();
    void onExportFRD(bool includeCalibration);
    void onLoadCalibration();
    void onClearCalibration();
    void onRunFit();
    void onApplyEQToPipeline();
    void onGenerateFIR();
    void onComputeSubwoofer();
    void toggleSidebar();

private:
    std::shared_ptr<PipelineStore> m_pipeline;
    MeasurementSession m_session;

    // Header Toolbar
    QPushButton* m_measureMenuBtn;
    QAction* m_newCapAction = nullptr;
    QAction* m_addCapAction = nullptr;
    QAction* m_newMockAction = nullptr;
    QAction* m_addMockAction = nullptr;
    QAction* m_importFrdAction = nullptr;
    QTabBar* m_paneTabBar;
    QToolButton* m_sidebarToggleBtn;

    // Center Plot Area & Positions Bar
    QStackedWidget* m_plotStackedWidget;
    EQDiagramWidget* m_frDiagramWidget;
    PhasePlotWidget* m_phasePlotWidget;
    ImpulseResponsePlotWidget* m_impulsePlotWidget;
    GroupDelayPlotWidget* m_groupDelayPlotWidget;
    WaterfallPlotWidget* m_waterfallWidget;

    QWidget* m_positionsContainer;
    QHBoxLayout* m_positionsChipsLayout;
    QPushButton* m_subwooferAssistBtn;

    // Collapsible Sidebar & Sections
    QWidget* m_sidebarWidget;
    bool m_sidebarVisible = true;

    // Audio Setup & Sweep Parameters
    QComboBox* m_micDeviceCombo;
    QComboBox* m_micChannelCombo;
    QComboBox* m_outputDeviceCombo;
    QComboBox* m_outputChannelCombo;
    QDoubleSpinBox* m_sweepF1Spin;
    QDoubleSpinBox* m_sweepF2Spin;
    QDoubleSpinBox* m_sweepDurationSpin;
    QLabel* m_calPathLabel;
    QPushButton* m_loadCalBtn;
    QToolButton* m_clearCalBtn;
    QPushButton* m_exportFrdBtn;
    QPushButton* m_exportCalFrdBtn;

    // Target & Analysis
    QComboBox* m_targetPresetCombo;
    QComboBox* m_smoothingCombo;
    QComboBox* m_fdwCombo;

    // Modal Region
    QCheckBox* m_modalModeCheck;
    QWidget* m_modalParamsContainer;
    QComboBox* m_schroederCombo;
    QComboBox* m_modalMinQCombo;

    // PEQ Design
    QComboBox* m_bandCountCombo;
    QPushButton* m_generatePeqBtn;
    QPushButton* m_addToEqPresetsBtn;

    // FIR Convolution Design
    QComboBox* m_firKindCombo;
    QComboBox* m_firTapCombo;
    QWidget* m_phaseBlendContainer;
    QSlider* m_phaseBlendSlider;
    QLabel* m_phaseBlendValueLabel;
    QPushButton* m_addToFirPresetsBtn;

    // Status bar
    QLabel* m_statusLabel;

    void setupUi();
    QWidget* createHeaderToolbar();
    QWidget* createMainArea();
    QWidget* createSidebar();

    void populateAudioDevices();
    void updateMicChannels();
    void updateOutputChannels();
};

#endif // ROOM_CORRECTION_DLG_H
