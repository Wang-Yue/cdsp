#ifndef ROOM_CORRECTION_DLG_H
#define ROOM_CORRECTION_DLG_H

#include "room_correction/MeasurementSession.h"
#include "models/PipelineStore.h"
#include "ui/EQDiagramWidget.h"
#include "ui/WaterfallPlotWidget.h"
#include <QDialog>
#include <QTabWidget>
#include <QListWidget>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <memory>

class RoomCorrectionDlg : public QDialog {
    Q_OBJECT

public:
    RoomCorrectionDlg(
        std::shared_ptr<PipelineStore> pipeline,
        QWidget* parent = nullptr
    );

private slots:
    void refreshSessionUi();
    void onGenerateMock();
    void onImportFRD();
    void onExportFRD();
    void onRunFit();
    void onApplyEQToPipeline();
    void onGenerateFIR();
    void onComputeSubwoofer();

private:
    std::shared_ptr<PipelineStore> m_pipeline;
    MeasurementSession m_session;

    QTabWidget* m_tabWidget;

    // Measurement Tab Widgets
    QListWidget* m_positionsList;
    EQDiagramWidget* m_frDiagramWidget;

    // Fit Tab Widgets
    QSpinBox* m_bandCountSpin;
    QDoubleSpinBox* m_maxGainSpin;
    QCheckBox* m_modalCheck;
    QDoubleSpinBox* m_schroederSpin;
    QComboBox* m_targetPresetCombo;

    // Subwoofer Tab Widgets
    QLabel* m_subResultLabel;

    // Waterfall Tab Widgets
    WaterfallPlotWidget* m_waterfallWidget;

    // FIR Tab Widgets
    QComboBox* m_firKindCombo;
    QSpinBox* m_firTapSpin;

    QLabel* m_statusLabel;

    void setupUi();
    void setupMeasurementTab(QWidget* tab);
    void setupFitTab(QWidget* tab);
    void setupSubwooferTab(QWidget* tab);
    void setupWaterfallTab(QWidget* tab);
    void setupFIRTab(QWidget* tab);
};

#endif // ROOM_CORRECTION_DLG_H
