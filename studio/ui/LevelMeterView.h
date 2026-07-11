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

class LevelMeterView : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeterView(QWidget* parent = nullptr);

    void setLevelState(LevelState* levelState) { m_levelState = levelState; }
    void setLevels(const std::vector<float>& rms, const std::vector<float>& peak, const QString& title = "Meters");

    QSize sizeHint() const override { return QSize(300, 120); }
    QSize minimumSizeHint() const override { return QSize(180, 90); }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    LevelState* m_levelState = nullptr;
    std::vector<float> m_rms;
    std::vector<float> m_peak;
    std::vector<float> m_peakHold;
    QString m_title;
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
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<MonitoringController> m_monitoring;
    std::shared_ptr<DSPEngineController> m_dsp;
    QLabel* m_statusLabel;
    QWidget* m_statusDot;
};

#endif // LEVEL_METER_VIEW_H
