#ifndef EQ_PRESET_DETAIL_VIEW_H
#define EQ_PRESET_DETAIL_VIEW_H

#include "models/DSPEngineController.h"
#include "models/EQPreset.h"
#include "models/PipelineStore.h"
#include "ui/EQDiagramWidget.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QWidget>
#include <memory>

class EQPresetDetailView : public QWidget {
    Q_OBJECT

public:
    EQPresetDetailView(EQPreset preset, std::shared_ptr<PipelineStore> pipeline,
                       std::shared_ptr<DSPEngineController> dspController = nullptr, QWidget* parent = nullptr);

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    EQPreset m_preset;
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<DSPEngineController> m_dspController;

    QLineEdit* m_nameEdit;
    QSlider* m_preampSlider;
    QDoubleSpinBox* m_preampSpin;
    QTabBar* m_modeTabBar;

    QStackedWidget* m_modeStack;

    // Mode 0: Diagram
    EQDiagramWidget* m_diagramWidget;
    QWidget* m_bandChipsWidget = nullptr;
    class QHBoxLayout* m_chipLayout = nullptr;

    // Mode 1: Bands Table Form
    QTableWidget* m_bandsTable;

    // Mode 2: Raw CSV Text Editor
    QTextEdit* m_csvTextEdit;
    QLabel* m_csvStatusLabel;

    bool m_isRefreshing = false;

    void setupUi();
    void updateBandChipsBar();
};

#endif // EQ_PRESET_DETAIL_VIEW_H
