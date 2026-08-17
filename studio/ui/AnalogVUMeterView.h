#ifndef ANALOG_VU_METER_VIEW_H
#define ANALOG_VU_METER_VIEW_H

#include "models/LevelState.h"
#include "ui/VUSettings.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QWidget>
#include <vector>

class AnalogVUMeter : public QWidget {
    Q_OBJECT

public:
    explicit AnalogVUMeter(int channelIndex, const VUSettings& settings, QWidget* parent = nullptr);

    void setChannelIndex(int idx) {
        m_channelIndex = idx;
        update();
    }
    void setLevel(float dbFS);
    void setVUSettings(const VUSettings& settings);
    void setGainCalibration(float gainDb);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    int m_channelIndex = 0;
    float m_levelDb = -100.0f;
    float m_gainCalibrationDb = 0.0f;
    VUSettings m_settings;

    QPixmap m_cachedDialPixmap;
    QSize m_cachedDialSize;
    float m_cachedScale = 0.0f;
    VUTheme m_cachedTheme = VUTheme::VintageAmber;

    float computeAngleForLevel(float dbFS) const;
    void renderDialBackground(QPixmap& pixmap, const QSize& size, float scale);
};

class AnalogVUMeterView : public QWidget {
    Q_OBJECT

public:
    explicit AnalogVUMeterView(QWidget* parent = nullptr);
    ~AnalogVUMeterView() override;

    void setLevelState(LevelState* levelState);
    void setLevels(const std::vector<float>& levels);
    void setLevelDB(float leftDB, float rightDB);
    void setVUSettings(const VUSettings& settings);
    VUSettings vuSettings() const { return m_settings; }

    void setGainCalibration(float gainDb);
    float gainCalibration() const { return m_gainCalibrationDb; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    LevelState* m_levelState = nullptr;
    std::vector<float> m_levels;
    float m_gainCalibrationDb = 0.0f;
    VUSettings m_settings;

    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_canvasWidget = nullptr;
    QHBoxLayout* m_canvasLayout = nullptr;
    std::vector<AnalogVUMeter*> m_meters;

    void updateChannelMeters();
};

#endif // ANALOG_VU_METER_VIEW_H
