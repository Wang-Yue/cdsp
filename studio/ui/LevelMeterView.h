#ifndef LEVEL_METER_VIEW_H
#define LEVEL_METER_VIEW_H

#include "models/LevelState.h"
#include <QWidget>
#include <QPainter>

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

#endif // LEVEL_METER_VIEW_H
