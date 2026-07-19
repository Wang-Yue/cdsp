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
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
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

    QComboBox* m_themeCombo;
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

    QComboBox* m_sourceCombo;
    QComboBox* m_channelCombo;
    QSpinBox* m_binsSpin;
    QComboBox* m_windowCombo;
    QComboBox* m_smoothingCombo;
    QComboBox* m_decayCombo;
    LogRangeSlider* m_rangeSlider;
    QLabel* m_rangeLbl;
    LogRangeSlider* m_dbRangeSlider;
    QLabel* m_dbRangeLbl;

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

    QComboBox* m_sourceCombo;
    QComboBox* m_channelCombo;
    QSpinBox* m_binsSpin;
    QComboBox* m_modeCombo;
    QComboBox* m_paletteCombo;

    void setupUi();
};

class VectorScopeDetailView : public QWidget {
    Q_OBJECT

public:
    explicit VectorScopeDetailView(std::shared_ptr<VectorScopeEngine> engine,
                                   std::shared_ptr<AudioDeviceManager> devices = nullptr, QWidget* parent = nullptr);

private:
    std::shared_ptr<VectorScopeEngine> m_engine;
    std::shared_ptr<AudioDeviceManager> m_devices;
    VectorScopeView* m_vectorView;

    QComboBox* m_sourceCombo;
    QComboBox* m_channelLCombo;
    QComboBox* m_channelRCombo;
    QSpinBox* m_framesSpin;
    QComboBox* m_modeCombo;
    QComboBox* m_decayCombo;
    QCheckBox* m_autoScaleCheck;

    void setupUi();
};

#endif // VISUALIZER_DETAIL_VIEWS_H
