#ifndef STAGE_DETAIL_VIEW_H
#define STAGE_DETAIL_VIEW_H

#include "models/PipelineStage.h"
#include "models/PipelineStore.h"
#include "models/DSPEngineController.h"
#include "ui/EQDiagramWidget.h"
#include "ui/ConvolutionIRPlot.h"

#include <QWidget>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QTableWidget>
#include <QLabel>
#include <memory>

class StageDetailView : public QWidget {
    Q_OBJECT

public:
    StageDetailView(
        size_t stageIndex,
        std::shared_ptr<PipelineStore> pipeline,
        std::shared_ptr<DSPEngineController> dspController,
        QWidget* parent = nullptr
    );

private slots:
    void refreshUi();
    void applyConfig();

private:
    size_t m_stageIndex;
    std::shared_ptr<PipelineStore> m_pipeline;
    std::shared_ptr<DSPEngineController> m_dspController;

    QLineEdit* m_nameEdit;
    QCheckBox* m_enabledCheck;

    // Stage Options Panels Container
    QWidget* m_optionsContainer;

    void setupUi();
    void buildStageOptionsUi();
};

#endif // STAGE_DETAIL_VIEW_H
