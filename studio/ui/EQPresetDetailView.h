#ifndef EQ_PRESET_DETAIL_VIEW_H
#define EQ_PRESET_DETAIL_VIEW_H

#include "models/EQPreset.h"
#include "models/PipelineStore.h"
#include "ui/EQDiagramWidget.h"

#include <QWidget>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QPushButton>
#include <QComboBox>
#include <QStackedWidget>
#include <QTextEdit>
#include <QLabel>
#include <memory>

#include <QSlider>
#include <QTabBar>

class EQPresetDetailView : public QWidget {
    Q_OBJECT

public:
    EQPresetDetailView(
        EQPreset preset,
        std::shared_ptr<PipelineStore> pipeline,
        QWidget* parent = nullptr
    );

    void setPreset(const EQPreset& preset);
    void setSpectrumEngine(std::shared_ptr<SpectrumEngine> spectrum);

private slots:
    void refreshUi();
    void onAddBand();
    void onDeleteBand(int row);
    void onExportCSV();
    void onImportCSV();
    void onApplyCSV();
    void onCopyCSV();

private:
    EQPreset m_preset;
    std::shared_ptr<PipelineStore> m_pipeline;

    QLineEdit* m_nameEdit;
    QSlider* m_preampSlider;
    QDoubleSpinBox* m_preampSpin;
    QTabBar* m_modeTabBar;

    QStackedWidget* m_modeStack;

    // Mode 0: Diagram
    EQDiagramWidget* m_diagramWidget;

    // Mode 1: Bands Table Form
    QTableWidget* m_bandsTable;

    // Mode 2: Raw CSV Text Editor
    QTextEdit* m_csvTextEdit;
    QLabel* m_csvStatusLabel;

    void setupUi();
};

#endif // EQ_PRESET_DETAIL_VIEW_H
