#ifndef VISUALIZER_DETAIL_VIEWS_H
#define VISUALIZER_DETAIL_VIEWS_H

#include "models/AudioDeviceManager.h"
#include "models/MonitoringController.h"
#include "models/SpectrogramEngine.h"
#include "models/SpectrumEngine.h"
#include "models/VectorScopeEngine.h"
#include "ui/AnalogVUMeterView.h"
#include "ui/LogRangeSlider.h"
#include "ui/SpectrogramView.h"
#include "ui/SpectrumView.h"
#include "ui/VectorScopeView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

class AnalogVUDetailView : public QWidget {
    Q_OBJECT

public:
    explicit AnalogVUDetailView(std::shared_ptr<MonitoringController> monitoring, QWidget* parent = nullptr);

private slots:
    void refreshUi();
    void resetDefaults();

private:
    std::shared_ptr<MonitoringController> m_monitoring;
    AnalogVUMeterView* m_vuMeter;
    VUSettings m_settings;

    QSlider* m_radiusSlider;
    QLabel* m_radiusLbl;
    QSlider* m_pivotYSlider;
    QLabel* m_pivotYLbl;
    QSlider* m_needleExtSlider;
    QLabel* m_needleExtLbl;
    QSlider* m_ambientGlowSlider;
    QLabel* m_ambientGlowLbl;
    QSlider* m_hotSpotSlider;
    QLabel* m_hotSpotLbl;
    QSlider* m_lightWashSlider;
    QLabel* m_lightWashLbl;
    QComboBox* m_vuThemeCombo = nullptr;

    void setupUi();
};

class SpectrumDetailView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumDetailView(std::shared_ptr<SpectrumEngine> engine, std::shared_ptr<AudioDeviceManager> devices,
                                QWidget* parent = nullptr);

private:
    std::shared_ptr<SpectrumEngine> m_engine;
    std::shared_ptr<AudioDeviceManager> m_devices;
    SpectrumView* m_spectrumView;

    QTabBar* m_sourceTabBar;
    QComboBox* m_channelCombo;
    QSpinBox* m_binsSpin;
    LogRangeSlider* m_rangeSlider;
    QLabel* m_rangeLbl;

    void setupUi();
};

class SpectrogramDetailView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrogramDetailView(std::shared_ptr<SpectrogramEngine> engine,
                                   std::shared_ptr<AudioDeviceManager> devices, QWidget* parent = nullptr);

private:
    std::shared_ptr<SpectrogramEngine> m_engine;
    std::shared_ptr<AudioDeviceManager> m_devices;
    SpectrogramView* m_spectrogramView;

    QTabBar* m_sourceTabBar;
    QComboBox* m_channelCombo;
    QSpinBox* m_binsSpin;
    QTabBar* m_modeTabBar;

    void setupUi();
};

using SpectroscopeDetailView = SpectrogramDetailView;

class VectorScopeDetailView : public QWidget {
    Q_OBJECT

public:
    explicit VectorScopeDetailView(std::shared_ptr<VectorScopeEngine> engine,
                                   std::shared_ptr<AudioDeviceManager> devices = nullptr, QWidget* parent = nullptr);

private:
    std::shared_ptr<VectorScopeEngine> m_engine;
    std::shared_ptr<AudioDeviceManager> m_devices;
    VectorScopeView* m_vectorView;

    QTabBar* m_sourceTabBar;
    QSpinBox* m_framesSpin;
    QTabBar* m_modeTabBar;
    QCheckBox* m_autoScaleCheck;

    void setupUi();
};

#endif // VISUALIZER_DETAIL_VIEWS_H
