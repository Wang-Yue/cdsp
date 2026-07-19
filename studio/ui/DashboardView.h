#ifndef DASHBOARD_VIEW_H
#define DASHBOARD_VIEW_H

#include "models/DSPEngineController.h"
#include "models/MonitoringController.h"
#include "models/SpectrogramEngine.h"
#include "models/SpectrumEngine.h"
#include "models/VectorScopeEngine.h"
#include "ui/AnalogVUMeterView.h"
#include "ui/DSPDetailedSignalGraphCard.h"
#include "ui/LevelMeterView.h"
#include "ui/PipelineOverviewWidget.h"
#include "ui/SpectrogramView.h"
#include "ui/SpectrumView.h"
#include "ui/VectorScopeView.h"

#include <QGroupBox>
#include <QPushButton>
#include <QSlider>
#include <QWidget>
#include <vector>

class DashboardView : public QWidget {
    Q_OBJECT

public:
    DashboardView(std::shared_ptr<MonitoringController> monitoring, std::shared_ptr<DSPEngineController> dspController,
                  std::shared_ptr<SpectrumEngine> spectrumEngine = nullptr,
                  std::shared_ptr<SpectrogramEngine> spectrogramEngine = nullptr,
                  std::shared_ptr<VectorScopeEngine> vectorScopeEngine = nullptr, QWidget* parent = nullptr);

private slots:
    void refreshMeters();
    void updateVisibility();
    void updateFaderUi();
    void updateSignalChain();

private:
    std::shared_ptr<MonitoringController> m_monitoring;
    std::shared_ptr<DSPEngineController> m_dspController;
    std::shared_ptr<SpectrumEngine> m_spectrumEngine;
    std::shared_ptr<SpectrogramEngine> m_spectrogramEngine;
    std::shared_ptr<VectorScopeEngine> m_vectorScopeEngine;

    PipelineOverviewWidget* m_pipelineOverviewWidget = nullptr;
    DSPDetailedSignalGraphCard* m_signalGraphCard = nullptr;

    QGroupBox* m_levelMetersGroup = nullptr;
    QGroupBox* m_analogVUGroup = nullptr;
    QGroupBox* m_spectrumGroup = nullptr;
    QGroupBox* m_spectrogramGroup = nullptr;
    QGroupBox* m_vectorScopeGroup = nullptr;
    QGroupBox* m_faderGroup = nullptr;

    LevelMeterView* m_captureMeters = nullptr;
    LevelMeterView* m_playbackMeters = nullptr;
    SpectrumView* m_spectrumView = nullptr;
    SpectrogramView* m_spectrogramView = nullptr;
    VectorScopeView* m_vectorScopeView = nullptr;
    AnalogVUMeterView* m_analogVUView = nullptr;

    struct FaderRowWidgets {
        Fader fader;
        QLabel* label;
        QPushButton* muteBtn;
        QSlider* slider;
        QLabel* gainValueLabel;
    };
    std::vector<FaderRowWidgets> m_faderRows;

    void setupUi();
};

#endif // DASHBOARD_VIEW_H
