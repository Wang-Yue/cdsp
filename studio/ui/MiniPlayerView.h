#ifndef MINI_PLAYER_VIEW_H
#define MINI_PLAYER_VIEW_H

#include "models/DSPEngineController.h"
#include "models/AudioSettings.h"
#include "models/MonitoringController.h"
#include "ui/LevelMeterView.h"
#include "ui/SpectrumView.h"
#include "ui/SpectrogramView.h"
#include "ui/VectorScopeView.h"
#include "ui/AnalogVUMeterView.h"

#include <QWidget>
#include <QSlider>
#include <QPushButton>
#include <QStackedWidget>
#include <QComboBox>
#include <QLabel>
#include <QPoint>
#include <memory>

class MiniPlayerView : public QWidget {
    Q_OBJECT

public:
    MiniPlayerView(
        std::shared_ptr<DSPEngineController> dsp,
        std::shared_ptr<AudioSettings> settings,
        std::shared_ptr<MonitoringController> monitoring,
        QWidget* parent = nullptr
    );

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private slots:
    void refreshMeters();
    void onFaderChanged(int index);

private:
    std::shared_ptr<DSPEngineController> m_dsp;
    std::shared_ptr<AudioSettings> m_settings;
    std::shared_ptr<MonitoringController> m_monitoring;

    QPoint m_dragPosition;
    QComboBox* m_faderCombo;
    QPushButton* m_playStopBtn;
    QPushButton* m_muteBtn;
    QSlider* m_volSlider;
    QLabel* m_volValueLabel;

    QStackedWidget* m_viewStack;
    LevelMeterView* m_metersView;
    SpectrumView* m_spectrumView;
    SpectrogramView* m_spectrogramView;
    VectorScopeView* m_vectorScopeView;
    AnalogVUMeterView* m_analogVUView;

    Fader currentFader() const;
    void setupUi();
};

#endif // MINI_PLAYER_VIEW_H
