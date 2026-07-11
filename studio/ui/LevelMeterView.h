#ifndef LEVEL_METER_VIEW_H
#define LEVEL_METER_VIEW_H

#include "models/LevelState.h"
#include "models/DSPEngineController.h"
#include <QWidget>
#include <QPainter>
#include <QLabel>
#include <QHBoxLayout>
#include <memory>

class MonitoringController;

class LevelMeterView : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeterView(QWidget* parent = nullptr);

    void setLevels(const std::vector<float>& rms, const std::vector<float>& peak, const QString& title = "Meters");

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<float> m_rms;
    std::vector<float> m_peak;
    QString m_title;
};

class CompactLevelMeterBar : public QWidget {
    Q_OBJECT

public:
    explicit CompactLevelMeterBar(std::shared_ptr<MonitoringController> monitoring,
                                 std::shared_ptr<DSPEngineController> dsp,
                                 QWidget* parent = nullptr);
    void updateState();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<MonitoringController> m_monitoring;
    std::shared_ptr<DSPEngineController> m_dsp;
    QLabel* m_statusLabel;
    QWidget* m_statusDot;
};

#endif // LEVEL_METER_VIEW_H
