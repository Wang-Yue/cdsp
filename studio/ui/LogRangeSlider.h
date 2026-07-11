#ifndef LOG_RANGE_SLIDER_H
#define LOG_RANGE_SLIDER_H

#include <QWidget>
#include <QPainter>
#include <QMouseEvent>

class LogRangeSlider : public QWidget {
    Q_OBJECT

public:
    explicit LogRangeSlider(QWidget* parent = nullptr);

    double minFreq() const { return m_minFreq; }
    double maxFreq() const { return m_maxFreq; }

    void setRange(double minFreq, double maxFreq);

signals:
    void rangeChanged(double minFreq, double maxFreq);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    double m_minFreq = 20.0;
    double m_maxFreq = 20000.0;
    int m_activeHandle = 0; // 0: none, 1: min, 2: max

    double posToFreq(int x) const;
    int freqToPos(double freq) const;
};

#endif // LOG_RANGE_SLIDER_H
