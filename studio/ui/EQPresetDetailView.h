#ifndef EQ_PRESET_DETAIL_VIEW_H
#define EQ_PRESET_DETAIL_VIEW_H

#include "models/DSPEngineController.h" // for DSPEngineController
#include "models/EQPreset.h"            // for EQPreset
#include "models/PipelineStore.h"       // for PipelineStore
#include "models/SpectrumEngine.h"      // for SpectrumEngine
#include "ui/EQDiagramWidget.h"         // for EQDiagramWidget

#include <QDoubleSpinBox> // for QDoubleSpinBox
#include <QLabel>         // for QLabel
#include <QLineEdit>      // for QLineEdit
#include <QObject>        // for Q_OBJECT, slots
#include <QPushButton>    // for QPushButton
#include <QSlider>        // for QSlider
#include <QTabWidget>     // for QTabWidget
#include <QTableWidget>   // for QTableWidget
#include <QTextEdit>      // for QTextEdit
#include <QWidget>        // for QWidget
#include <memory>         // for shared_ptr

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

    QLineEdit* m_nameEdit = nullptr;
    QSlider* m_preampSlider = nullptr;
    QDoubleSpinBox* m_preampSpin = nullptr;
    QSlider* m_formPreampSlider = nullptr;
    QDoubleSpinBox* m_formPreampSpin = nullptr;

    QTabWidget* m_tabWidget = nullptr;

    // Tab 0: Diagram
    EQDiagramWidget* m_diagramWidget = nullptr;
    QWidget* m_bandChipsWidget = nullptr;
    class QHBoxLayout* m_chipLayout = nullptr;

    // Tab 1: Bands Table Form
    QTableWidget* m_bandsTable = nullptr;

    // Tab 2: Raw CSV Text Editor
    QTextEdit* m_csvTextEdit = nullptr;
    QLabel* m_csvErrorLabel = nullptr;
    QPushButton* m_csvCopyBtn = nullptr;

    bool m_isRefreshing = false;

    void setupUi();
    void updateBandChipsBar();
    void applyConfig();
};

#endif // EQ_PRESET_DETAIL_VIEW_H
