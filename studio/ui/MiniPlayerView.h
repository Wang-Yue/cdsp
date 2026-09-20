#ifndef MINI_PLAYER_VIEW_H
#define MINI_PLAYER_VIEW_H

#include "config/DSPConfigTypes.h"       // for Fader, ProcessingState
#include "models/AudioSettings.h"        // for AudioSettings
#include "models/DSPEngineController.h"  // for DSPEngineController
#include "models/MonitoringController.h" // for MonitoringController
#include "ui/AnalogVUMeterView.h"        // for AnalogVUMeterView
#include "ui/LevelMeterView.h"           // for LevelMeterView
#include "ui/SpectrogramView.h"          // for SpectrogramView
#include "ui/SpectrumView.h"             // for SpectrumView
#include "ui/VectorScopeView.h"          // for VectorScopeView

#include <QGraphicsOpacityEffect> // for QGraphicsOpacityEffect
#include <QLabel>                 // for QLabel
#include <QObject>                // for Q_OBJECT, signals, slots
#include <QPoint>                 // for QPoint
#include <QPushButton>            // for QPushButton
#include <QRect>                  // for QRect
#include <QSlider>                // for QSlider
#include <QStackedWidget>         // for QStackedWidget
#include <QWidget>                // for QWidget
#include <memory>                 // for shared_ptr
#include <vector>                 // for vector

class MiniPlayerView : public QWidget {
    Q_OBJECT

public:
    MiniPlayerView(std::shared_ptr<DSPEngineController> dsp, std::shared_ptr<AudioSettings> settings,
                   std::shared_ptr<MonitoringController> monitoring, QWidget* parent = nullptr);

signals:
    void requestRestoreMainWindow();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void refreshMeters();
    void onFaderChanged(int index);
    void updateEngineStatus(ProcessingState state);
    void closeAndRestoreMain();

private:
    enum class ResizeEdge {
        None = 0,
        Left = 1,
        Right = 2,
        Top = 4,
        Bottom = 8,
        TopLeft = 4 | 1,
        TopRight = 4 | 2,
        BottomLeft = 8 | 1,
        BottomRight = 8 | 2
    };

    std::shared_ptr<DSPEngineController> m_dsp;
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<MonitoringController> m_monitoring;

    QPoint m_dragPosition;
    bool m_isDragging = false;
    bool m_isResizing = false;
    ResizeEdge m_activeResizeEdge = ResizeEdge::None;
    QRect m_dragStartGeometry;
    QPoint m_dragStartPos;

    QGraphicsOpacityEffect* m_headerOpacityEffect = nullptr;
    QPushButton* m_playStopBtn = nullptr;
    QPushButton* m_muteBtn = nullptr;
    QSlider* m_volSlider = nullptr;
    QLabel* m_volValueLabel = nullptr;

    QStackedWidget* m_viewStack = nullptr;
    QWidget* m_pipelineMiniCard = nullptr;
    LevelMeterView* m_metersView = nullptr;
    SpectrumView* m_spectrumView = nullptr;
    SpectrogramView* m_spectrogramView = nullptr;
    VectorScopeView* m_vectorScopeView = nullptr;
    AnalogVUMeterView* m_analogVUView = nullptr;

    std::vector<QPushButton*> m_modeBtns;

    Fader currentFader() const;
    void setupUi();
    void buildMiniPipelineUi();
    void updateModeButtonStyles(int activeIndex);
    ResizeEdge hitTestBorder(const QPoint& globalPos) const;
    void updateResizeCursor(ResizeEdge edge);
};

#endif // MINI_PLAYER_VIEW_H
