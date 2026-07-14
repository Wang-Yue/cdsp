#ifndef PIPELINE_OVERVIEW_WIDGET_H
#define PIPELINE_OVERVIEW_WIDGET_H

#include "models/DSPEngineController.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

class PipelineOverviewWidget : public QGroupBox {
    Q_OBJECT

public:
    explicit PipelineOverviewWidget(std::shared_ptr<DSPEngineController> dspController, QWidget* parent = nullptr);

public slots:
    void rebuildOverview();

private:
    std::shared_ptr<DSPEngineController> m_dspController;

    bool m_showElementaryDetails = true;

    QLabel* m_headerTitle = nullptr;
    QLabel* m_warningBadge = nullptr;
    QLabel* m_statsLabel = nullptr;
    QPushButton* m_toggleDetailsBtn = nullptr;

    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_canvasWidget = nullptr;
    QHBoxLayout* m_canvasLayout = nullptr;

    void setupUi();
    QString formatSampleRate(int rate) const;
    QString readableFilterName(const std::string& rawName, const PipelineStage& stage) const;
    QString readableMixerOrProcessorName(const std::string& rawName, const PipelineStage& stage) const;
};

#endif // PIPELINE_OVERVIEW_WIDGET_H
