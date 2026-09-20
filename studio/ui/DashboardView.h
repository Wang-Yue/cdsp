#ifndef DASHBOARD_VIEW_H
#define DASHBOARD_VIEW_H

#include "config/DSPConfigTypes.h"         // for Fader
#include "models/DSPEngineController.h"    // for DSPEngineController
#include "models/MonitoringController.h"   // for MonitoringController
#include "models/SpectrogramEngine.h"      // for SpectrogramEngine
#include "models/SpectrumEngine.h"         // for SpectrumEngine
#include "models/VectorScopeEngine.h"      // for VectorScopeEngine
#include "ui/AnalogVUMeterView.h"          // for AnalogVUMeterView
#include "ui/DSPDetailedSignalGraphCard.h" // for DSPDetailedSignalGraphCard
#include "ui/LevelMeterView.h"             // for LevelMetersCard
#include "ui/PipelineOverviewWidget.h"     // for PipelineOverviewWidget
#include "ui/SpectrogramView.h"            // for SpectrogramView
#include "ui/SpectrumView.h"               // for SpectrumView
#include "ui/VectorScopeView.h"            // for VectorScopeView

#include <QGroupBox>   // for QGroupBox
#include <QLabel>      // for QLabel
#include <QObject>     // for Q_OBJECT, slots
#include <QPushButton> // for QPushButton
#include <QSlider>     // for QSlider
#include <QWidget>     // for QWidget
#include <memory>      // for shared_ptr
#include <vector>      // for vector

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
    LevelMetersCard* m_levelMetersCard = nullptr;
    QGroupBox* m_analogVUGroup = nullptr;
    QGroupBox* m_spectrumGroup = nullptr;
    QGroupBox* m_spectrogramGroup = nullptr;
    QGroupBox* m_vectorScopeGroup = nullptr;
    QGroupBox* m_faderGroup = nullptr;

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
