#ifndef PIPELINE_OVERVIEW_WIDGET_H
#define PIPELINE_OVERVIEW_WIDGET_H

#include "models/DSPEngineController.h"

#include <QColor>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <map>
#include <memory>
#include <optional>

class PipelineOverviewWidget;

class OverviewCanvasWidget : public QWidget {
    Q_OBJECT

public:
    explicit OverviewCanvasWidget(PipelineOverviewWidget* owner, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    PipelineOverviewWidget* m_owner;
};

class PipelineOverviewWidget : public QGroupBox {
    Q_OBJECT

public:
    explicit PipelineOverviewWidget(std::shared_ptr<DSPEngineController> dspController, QWidget* parent = nullptr);

    std::optional<int> hoveredChannel() const { return m_hoveredChannel; }
    int maxChannels() const { return m_lastMaxChannels; }
    QColor channelColor(int index) const;
    void paintCanvasWires(QWidget* canvasWidget);

public slots:
    void rebuildOverview();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::shared_ptr<DSPEngineController> m_dspController;

    bool m_showElementaryDetails = true;
    std::optional<int> m_hoveredChannel = std::nullopt;
    int m_lastMaxChannels = 2;

    QLabel* m_headerTitle = nullptr;
    QLabel* m_warningBadge = nullptr;
    QLabel* m_statsLabel = nullptr;
    QPushButton* m_addStageBtn = nullptr;
    QPushButton* m_toggleDetailsBtn = nullptr;

    QHBoxLayout* m_legendBarLayout = nullptr;
    QWidget* m_legendContainerWidget = nullptr;
    std::map<QObject*, int> m_legendBtnMap;

    QScrollArea* m_scrollArea = nullptr;
    OverviewCanvasWidget* m_canvasWidget = nullptr;
    QHBoxLayout* m_canvasLayout = nullptr;

    void setupUi();
    void buildAddStageMenu();
    void rebuildLegendBar(int maxChannels);
    void updateLegendPillStyle(QObject* obj, int ch, bool hovered);

    QIcon createChannelDotIcon(int ch, bool hovered) const;
    QString categoryColorHex(StageCategory cat) const;
    QString stepTypeColorHex(PipelineStepType type) const;
    QString stepTypeTitle(PipelineStepType type) const;
    QString formatSampleRate(int rate) const;
    QString readableFilterName(const std::string& rawName, const PipelineStage& stage) const;
    QString readableMixerOrProcessorName(const std::string& rawName, const PipelineStage& stage) const;
};

#endif // PIPELINE_OVERVIEW_WIDGET_H
