#ifndef DSP_DETAILED_SIGNAL_GRAPH_CARD_H
#define DSP_DETAILED_SIGNAL_GRAPH_CARD_H

#include "models/DSPEngineController.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <map>
#include <memory>
#include <set>
#include <vector>

// MARK: - Graph Elements Data Structures

struct GraphBlock {
    QString id;
    QString label;
    qreal x = 0;
    qreal y = 0;
    qreal width = 85;
    qreal height = 28;
    bool isChannelPort = false;
};

struct ContainerBox {
    QString id;
    QString label;
    qreal centerX = 0;
    qreal centerY = 0;
    qreal width = 76;
    qreal height = 40;
    int activeChannelsCount = 0;
    std::vector<QString> containedBlockIds;
};

struct GraphArrow {
    QString id;
    QString fromBlockId;
    QString toBlockId;
    QPointF fromFallback;
    QPointF toFallback;
    QString label;
};

// MARK: - Interactive 2D Painter Canvas Widget

class DSPGraphCanvas : public QWidget {
    Q_OBJECT

public:
    explicit DSPGraphCanvas(std::shared_ptr<DSPEngineController> dspController, QWidget* parent = nullptr);

    void resetLayout();
    bool hasCustomPositions() const { return !m_customPositions.empty(); }

public slots:
    void rebuildGraph();

signals:
    void layoutChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    std::shared_ptr<DSPEngineController> m_dspController;

    std::vector<GraphBlock> m_blocks;
    std::vector<ContainerBox> m_boxes;
    std::vector<GraphArrow> m_arrows;
    std::map<QString, GraphBlock> m_blocksMap;

    std::map<QString, QPointF> m_customPositions;

    QString m_draggedBlockId;
    QPointF m_dragStartPos;
    QPointF m_blockOriginPos;

    qreal m_xStep = 130;
    qreal m_yStep = 46;
    qreal m_blockWidth = 85;
    qreal m_blockHeight = 28;
    qreal m_canvasPadding = 30;
    qreal m_titleHeaderHeight = 36;

    QPointF getBlockPos(const GraphBlock& b, qreal originY) const;
    void calculateGraphLayout();
    QString readableFilterStepName(const std::string& rawName, const PipelineStage& stage) const;
    QString readableMixerTitle(const std::string& rawName, int inCh, int outCh) const;
};

// MARK: - Main Card Wrapper

class DSPDetailedSignalGraphCard : public QGroupBox {
    Q_OBJECT

public:
    explicit DSPDetailedSignalGraphCard(std::shared_ptr<DSPEngineController> dspController, QWidget* parent = nullptr);

public slots:
    void updateCard();
    void updateScrollHeight();

protected:
    void showEvent(QShowEvent* event) override;

private:
    std::shared_ptr<DSPEngineController> m_dspController;

    QLabel* m_activeStagesBadge = nullptr;
    QPushButton* m_resetLayoutBtn = nullptr;
    DSPGraphCanvas* m_canvas = nullptr;
    QScrollArea* m_scrollArea = nullptr;

    void setupUi();
};

#endif // DSP_DETAILED_SIGNAL_GRAPH_CARD_H
