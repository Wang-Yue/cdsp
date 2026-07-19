#ifndef ANALOG_VU_METER_VIEW_H
#define ANALOG_VU_METER_VIEW_H

#include "models/LevelState.h"
#include "ui/VUSettings.h"

#include <QPainter>
#include <QTimer>
#include <QWidget>

class AnalogVUMeterView : public QWidget {
    Q_OBJECT

public:
    explicit AnalogVUMeterView(QWidget* parent = nullptr);

    void setLevelState(LevelState* levelState) { m_levelState = levelState; }
    void setLevelDB(float leftDB, float rightDB);
    void setVUSettings(const VUSettings& settings);
    VUSettings vuSettings() const { return m_settings; }

    void setGainCalibration(float gainDb) {
        m_gainCalibrationDb = gainDb;
        update();
    }
    float gainCalibration() const { return m_gainCalibrationDb; }

    QSize sizeHint() const override { return QSize(360, 220); }
    QSize minimumSizeHint() const override { return QSize(200, 160); }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    LevelState* m_levelState = nullptr;
    float m_leftDB = -60.0f;
    float m_rightDB = -60.0f;
    float m_gainCalibrationDb = 0.0f; // Gain calibration knob offset (-12 to +12 dB)

    VUSettings m_settings;

    float computeAngleForLevel(float dbFS) const;
    void drawSingleVU(QPainter& p, const QRect& totalRect, float angle, const QString& label, float scale);
};

#endif // ANALOG_VU_METER_VIEW_H
