#ifndef ANALOG_VU_METER_VIEW_H
#define ANALOG_VU_METER_VIEW_H

#include <QWidget>
#include <QPainter>

class AnalogVUMeterView : public QWidget {
    Q_OBJECT

public:
    explicit AnalogVUMeterView(QWidget* parent = nullptr);

    void setLevelDB(float leftDB, float rightDB);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    float m_leftDB = -60.0f;
    float m_rightDB = -60.0f;

    float m_currentAngleL = -45.0f;
    float m_currentAngleR = -45.0f;

    void drawSingleVU(QPainter& p, const QRect& rect, float angle, const QString& label);
};

#endif // ANALOG_VU_METER_VIEW_H
