#ifndef ANALOG_VU_METER_VIEW_H
#define ANALOG_VU_METER_VIEW_H

#include <QWidget>
#include <QPainter>

#include <QWidget>
#include <QPainter>
#include <QTimer>

#include "models/LevelState.h"
#include "ui/VUSettings.h"

class AnalogVUMeterView : public QWidget {
    Q_OBJECT

public:
    explicit AnalogVUMeterView(QWidget* parent = nullptr);

    void setLevelState(LevelState* levelState) { m_levelState = levelState; }
    void setLevelDB(float leftDB, float rightDB);
    void setVUSettings(const VUSettings& settings);
    VUSettings vuSettings() const { return m_settings; }

    void setGainCalibration(float gainDb) { m_gainCalibrationDb = gainDb; update(); }
    float gainCalibration() const { return m_gainCalibrationDb; }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onAnimTick();

private:
    LevelState* m_levelState = nullptr;
    float m_leftDB = -60.0f;
    float m_rightDB = -60.0f;
    float m_gainCalibrationDb = 0.0f; // Gain calibration knob offset (-12 to +12 dB)

    float m_peakClipLHold = 0.0f;
    float m_peakClipRHold = 0.0f;

    float m_targetAngleL = -35.0f;
    float m_targetAngleR = -35.0f;
    float m_currentAngleL = -35.0f;
    float m_currentAngleR = -35.0f;
    float m_velocityL = 0.0f;
    float m_velocityR = 0.0f;

    VUSettings m_settings;
    QTimer m_animTimer;

    float computeAngleForLevel(float dbFS) const;
    void drawSingleVU(QPainter& p, const QRect& rect, float angle, const QString& label, float levelDb, bool isClipped);
};

#endif // ANALOG_VU_METER_VIEW_H
