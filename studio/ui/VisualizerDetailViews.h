#ifndef VISUALIZER_DETAIL_VIEWS_H
#define VISUALIZER_DETAIL_VIEWS_H

#include "models/AudioDeviceManager.h"   // for AudioDeviceManager
#include "models/MonitoringController.h" // for MonitoringController
#include "models/SpectrogramEngine.h"    // for SpectrogramEngine
#include "models/SpectrumEngine.h"       // for SpectrumEngine
#include "models/VectorScopeEngine.h"    // for VectorScopeEngine
#include "ui/AnalogVUMeterView.h"        // for AnalogVUMeterView
#include "ui/LogRangeSlider.h"           // for LogRangeSlider
#include "ui/SpectrogramView.h"          // for SpectrogramView
#include "ui/SpectrumView.h"             // for SpectrumView
#include "ui/VUSettings.h"               // for VUSettings
#include "ui/VectorScopeView.h"          // for VectorScopeView

#include <QCheckBox> // for QCheckBox
#include <QComboBox> // for QComboBox
#include <QLabel>    // for QLabel
#include <QObject>   // for Q_OBJECT, slots
#include <QSlider>   // for QSlider
#include <QSpinBox>  // for QSpinBox
#include <QTabBar>   // for QTabBar
#include <QWidget>   // for QWidget
#include <memory>    // for shared_ptr

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
    QComboBox* m_paletteCombo;

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
    QComboBox* m_windowCombo;
    QTabBar* m_modeTabBar;
    QCheckBox* m_autoScaleCheck;

    void setupUi();
};

#endif // VISUALIZER_DETAIL_VIEWS_H
