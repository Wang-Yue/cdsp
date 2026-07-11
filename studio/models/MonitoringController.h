#ifndef MONITORING_CONTROLLER_H
#define MONITORING_CONTROLLER_H

#include "engine/CDSPEngine.h"
#include "models/DSPEngineController.h"
#include "models/LevelState.h"
#include "models/SpectrumEngine.h"
#include "models/SpectrogramEngine.h"
#include "models/VectorScopeEngine.h"
#include <QObject>
#include <QTimer>
#include <memory>

class MonitoringController : public QObject {
    Q_OBJECT

public:
    MonitoringController(
        std::shared_ptr<CDSPEngine> engine,
        std::shared_ptr<DSPEngineController> dspController,
        std::shared_ptr<SpectrumEngine> spectrumEngine,
        std::shared_ptr<SpectrogramEngine> spectrogramEngine,
        std::shared_ptr<VectorScopeEngine> vectorScopeEngine,
        QObject* parent = nullptr
    );

    LevelState levelState;

    bool showLevelMetersInDashboard = true;
    bool showSpectrumInDashboard = true;
    bool showSpectrogramInDashboard = true;
    bool showVectorScopeInDashboard = true;
    bool showAnalogVUInDashboard = true;

    void start();
    void stop();

    double pollingRate() const { return m_pollingRate; }
    void setPollingRate(double rateHz) {
        m_pollingRate = rateHz;
        int intervalMs = static_cast<int>(1000.0 / std::max(1.0, rateHz));
        m_pollTimer.setInterval(intervalMs);
    }

    std::shared_ptr<SpectrumEngine> spectrumEngine() const { return m_spectrumEngine; }
    std::shared_ptr<SpectrogramEngine> spectrogramEngine() const { return m_spectrogramEngine; }
    std::shared_ptr<VectorScopeEngine> vectorScopeEngine() const { return m_vectorScopeEngine; }

signals:
    void levelsUpdated();

private slots:
    void onPollTimer();

private:
    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<DSPEngineController> m_dspController;
    std::shared_ptr<SpectrumEngine> m_spectrumEngine;
    std::shared_ptr<SpectrogramEngine> m_spectrogramEngine;
    std::shared_ptr<VectorScopeEngine> m_vectorScopeEngine;

    QTimer m_pollTimer;
    double m_pollingRate = 30.0;
};

#endif // MONITORING_CONTROLLER_H
