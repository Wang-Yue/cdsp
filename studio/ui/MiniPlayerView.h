#ifndef MINI_PLAYER_VIEW_H
#define MINI_PLAYER_VIEW_H

#include "models/AudioSettings.h"
#include "models/DSPEngineController.h"
#include "models/MonitoringController.h"
#include "ui/AnalogVUMeterView.h"
#include "ui/LevelMeterView.h"
#include "ui/SpectrogramView.h"
#include "ui/SpectrumView.h"
#include "ui/VectorScopeView.h"

#include <QComboBox>
#include <QLabel>
#include <QPoint>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QWidget>
#include <memory>

class MiniPlayerView : public QWidget {
    Q_OBJECT

public:
    MiniPlayerView(std::shared_ptr<DSPEngineController> dsp, std::shared_ptr<AudioSettings> settings,
                   std::shared_ptr<MonitoringController> monitoring, QWidget* parent = nullptr);

signals:
    void requestRestoreMainWindow();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private slots:
    void refreshMeters();
    void onFaderChanged(int index);
    void updateEngineStatus(ProcessingState state);

private:
    std::shared_ptr<DSPEngineController> m_dsp;
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<MonitoringController> m_monitoring;

    QPoint m_dragPosition;
    QComboBox* m_faderCombo = nullptr;
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

    Fader currentFader() const;
    void setupUi();
    void buildMiniPipelineUi();
};

#endif // MINI_PLAYER_VIEW_H
