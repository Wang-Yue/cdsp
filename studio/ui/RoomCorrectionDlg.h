#ifndef ROOM_CORRECTION_DLG_H
#define ROOM_CORRECTION_DLG_H

#include "models/PipelineStore.h"
#include "room_correction/MeasurementSession.h"
#include "ui/EQDiagramWidget.h"
#include "ui/GroupDelayPlotWidget.h"
#include "ui/ImpulseResponsePlotWidget.h"
#include "ui/MeasurementPositionRowWidget.h"
#include "ui/PhasePlotWidget.h"
#include "ui/WaterfallPlotWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <memory>

class RoomCorrectionDlg : public QDialog {
    Q_OBJECT

public:
    RoomCorrectionDlg(std::shared_ptr<PipelineStore> pipeline, QWidget* parent = nullptr);

private slots:
    void refreshSessionUi();
    void onGenerateMock();
    void onRecordHardwareMeasurement(bool append);
    void onImportFRD();
    void onExportFRD();
    void onLoadCalibration();
    void onClearCalibration();
    void onLoadTargetCurve();
    void onRunFit();
    void onApplyEQToPipeline();
    void onGenerateFIR();
    void onComputeSubwoofer();

private:
    std::shared_ptr<PipelineStore> m_pipeline;
    MeasurementSession m_session;

    QTabWidget* m_tabWidget;

    // Measurement & Multi-Plot Widgets
    QListWidget* m_positionsList;
    QTabBar* m_plotTabBar;
    QStackedWidget* m_plotStackedWidget;

    EQDiagramWidget* m_frDiagramWidget;
    PhasePlotWidget* m_phasePlotWidget;
    ImpulseResponsePlotWidget* m_impulsePlotWidget;
    GroupDelayPlotWidget* m_groupDelayPlotWidget;
    WaterfallPlotWidget* m_waterfallWidget;

    // Calibration & Target Curve
    QLabel* m_calStatusLabel;

    // Analysis / Controls
    QComboBox* m_fdwCombo;
    QComboBox* m_smoothingCombo;

    // Fit Tab Widgets
    QSpinBox* m_bandCountSpin;
    QDoubleSpinBox* m_maxGainSpin;
    QCheckBox* m_modalCheck;
    QDoubleSpinBox* m_schroederSpin;
    QComboBox* m_targetPresetCombo;

    // Subwoofer Tab Widgets
    QLabel* m_subResultLabel;

    // FIR Tab Widgets
    QComboBox* m_firKindCombo;
    QSpinBox* m_firTapSpin;
    QSlider* m_firPhaseBlendSlider;
    QLabel* m_firPhaseBlendLabel;

    QLabel* m_statusLabel;

    void setupUi();
    void setupMeasurementTab(QWidget* tab);
    void setupFitTab(QWidget* tab);
    void setupSubwooferTab(QWidget* tab);
    void setupFIRTab(QWidget* tab);
};

#endif // ROOM_CORRECTION_DLG_H
