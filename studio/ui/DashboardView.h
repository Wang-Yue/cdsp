#ifndef DASHBOARD_VIEW_H
#define DASHBOARD_VIEW_H

#include "models/MonitoringController.h"
#include "models/DSPEngineController.h"
#include "models/SpectrumEngine.h"
#include "models/SpectrogramEngine.h"
#include "models/VectorScopeEngine.h"
#include "ui/LevelMeterView.h"
#include "ui/SpectrumView.h"
#include "ui/SpectrogramView.h"
#include "ui/VectorScopeView.h"
#include "ui/AnalogVUMeterView.h"
#include <QWidget>
#include <QSlider>
#include <QPushButton>
#include <memory>

class DashboardView : public QWidget {
    Q_OBJECT

public:
    DashboardView(
        std::shared_ptr<MonitoringController> monitoring,
        std::shared_ptr<DSPEngineController> dspController,
        std::shared_ptr<SpectrumEngine> spectrumEngine = nullptr,
        std::shared_ptr<SpectrogramEngine> spectrogramEngine = nullptr,
        std::shared_ptr<VectorScopeEngine> vectorScopeEngine = nullptr,
        QWidget* parent = nullptr
    );

private slots:
    void refreshMeters();

private:
    std::shared_ptr<MonitoringController> m_monitoring;
    std::shared_ptr<DSPEngineController> m_dspController;
    std::shared_ptr<SpectrumEngine> m_spectrumEngine;
    std::shared_ptr<SpectrogramEngine> m_spectrogramEngine;
    std::shared_ptr<VectorScopeEngine> m_vectorScopeEngine;

    LevelMeterView* m_captureMeters;
    LevelMeterView* m_playbackMeters;
    SpectrumView* m_spectrumView;
    SpectrogramView* m_spectrogramView;
    VectorScopeView* m_vectorScopeView;
    AnalogVUMeterView* m_analogVUView;

    QSlider* m_mainFaderSlider;
    QPushButton* m_mainMuteBtn;

    void setupUi();
};

#endif // DASHBOARD_VIEW_H
