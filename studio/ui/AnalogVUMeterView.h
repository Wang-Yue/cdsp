#ifndef ANALOG_VU_METER_VIEW_H
#define ANALOG_VU_METER_VIEW_H

#include <QWidget>
#include <QPainter>

#include <QWidget>
#include <QPainter>
#include <QTimer>

#include "models/LevelState.h"

enum class VUTheme {
    VintageAmber,
    DarkStealth,
    WarmTube
};

struct VUSettings {
    double radiusScale = 1.20;
    double pivotY = 1.55;
    double needleExtension = 45.0;
    double ambientGlow = 0.5;
    double hotSpotAlpha = 0.5;
    double lightWash = 0.2;
    VUTheme theme = VUTheme::VintageAmber;

    void reset() {
        radiusScale = 1.20;
        pivotY = 1.55;
        needleExtension = 45.0;
        ambientGlow = 0.5;
        hotSpotAlpha = 0.5;
        lightWash = 0.2;
        theme = VUTheme::VintageAmber;
    }
};

class AnalogVUMeterView : public QWidget {
    Q_OBJECT

public:
    explicit AnalogVUMeterView(QWidget* parent = nullptr);

    void setLevelState(LevelState* levelState) { m_levelState = levelState; }
    void setLevelDB(float leftDB, float rightDB);
    void setVUSettings(const VUSettings& settings);
    VUSettings vuSettings() const { return m_settings; }

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

    float m_peakClipLHold = 0.0f;
    float m_peakClipRHold = 0.0f;

    float m_targetAngleL = -45.0f;
    float m_targetAngleR = -45.0f;
    float m_currentAngleL = -45.0f;
    float m_currentAngleR = -45.0f;

    VUSettings m_settings;
    QTimer m_animTimer;

    float computeAngleForLevel(float dbFS) const;
    void drawSingleVU(QPainter& p, const QRect& rect, float angle, const QString& label, bool isClipped);
};

#endif // ANALOG_VU_METER_VIEW_H
