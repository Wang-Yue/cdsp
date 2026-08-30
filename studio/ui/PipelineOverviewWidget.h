#ifndef PIPELINE_OVERVIEW_WIDGET_H
#define PIPELINE_OVERVIEW_WIDGET_H

#include "config/DSPConfigTypes.h"      // for PipelineStepType
#include "models/DSPEngineController.h" // for DSPEngineController
#include "models/PipelineStage.h"       // for PipelineStage, StageCategory

#include <QColor>      // for QColor
#include <QGroupBox>   // for QGroupBox
#include <QHBoxLayout> // for QHBoxLayout
#include <QIcon>       // for QIcon
#include <QLabel>      // for QLabel
#include <QObject>     // for QObject, Q_OBJECT, slots
#include <QPushButton> // for QPushButton
#include <QScrollArea> // for QScrollArea
#include <QString>     // for QString
#include <QWidget>     // for QWidget
#include <map>         // for map
#include <memory>      // for shared_ptr
#include <optional>    // for optional, nullopt, nullopt_t
#include <string>      // for string

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
    void updateScrollHeight();

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::shared_ptr<DSPEngineController> m_dspController;

    bool m_showElementaryDetails = false;
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
