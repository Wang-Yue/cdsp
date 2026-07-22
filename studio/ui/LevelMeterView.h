#ifndef LEVEL_METER_VIEW_H
#define LEVEL_METER_VIEW_H

#include "models/DSPEngineController.h"
#include "models/LevelState.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QWidget>
#include <memory>

class MonitoringController;
class MeterGroupWidget;

class LevelMeterView : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeterView(QWidget* parent = nullptr);

    void setLevelState(LevelState* levelState) {
        if (m_levelState == levelState)
            return;
        if (isVisible() && m_levelState && m_levelState->visibilityCount > 0)
            m_levelState->visibilityCount--;
        m_levelState = levelState;
        if (isVisible() && m_levelState)
            m_levelState->visibilityCount++;
    }
    void setIsCapture(bool isCapture) { m_isCapture = isCapture; }
    bool isCapture() const { return m_isCapture; }
    void setLevels(const std::vector<float>& rms, const std::vector<float>& peak, const QString& title = "Meters");

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    LevelState* m_levelState = nullptr;
    std::vector<float> m_rms;
    std::vector<float> m_peak;
    QString m_title;
    bool m_isCapture = false;
    bool m_hasExplicitLevels = false;
};

class CompactLevelMeterBar : public QWidget {
    Q_OBJECT

public:
    explicit CompactLevelMeterBar(std::shared_ptr<MonitoringController> monitoring,
                                  std::shared_ptr<DSPEngineController> dsp, QWidget* parent = nullptr);
    void updateState();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    std::shared_ptr<MonitoringController> m_monitoring;
    std::shared_ptr<DSPEngineController> m_dsp;
    QLabel* m_statusLabel;
    QWidget* m_statusDot;
    MeterGroupWidget* m_captureGroup = nullptr;
    MeterGroupWidget* m_playbackGroup = nullptr;
};

#endif // LEVEL_METER_VIEW_H
