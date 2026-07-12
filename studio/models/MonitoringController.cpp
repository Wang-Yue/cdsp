#include "models/MonitoringController.h"

MonitoringController::MonitoringController(std::shared_ptr<CDSPEngine> engine,
                                           std::shared_ptr<DSPEngineController> dspController,
                                           std::shared_ptr<SpectrumEngine> spectrumEngine,
                                           std::shared_ptr<SpectrogramEngine> spectrogramEngine,
                                           std::shared_ptr<VectorScopeEngine> vectorScopeEngine, QObject* parent)
    : QObject(parent), m_engine(engine), m_dspController(dspController), m_spectrumEngine(spectrumEngine),
      m_spectrogramEngine(spectrogramEngine), m_vectorScopeEngine(vectorScopeEngine) {

    connect(&m_pollTimer, &QTimer::timeout, this, &MonitoringController::onPollTimer);
    m_pollTimer.setInterval(33); // ~30 FPS polling
}

void MonitoringController::start() {
    m_pollTimer.start();
}

void MonitoringController::stop() {
    m_pollTimer.stop();
}

void MonitoringController::onPollTimer() {
    if (!m_engine || !m_dspController)
        return;
    StateUpdate st = m_engine->getStatus();
    if (st.state != m_dspController->status || st.stopReason.type != m_dspController->lastStopReason.type) {
        m_dspController->updateStatus(st);
    }

    if (st.state != ProcessingState::Running) {
        size_t capCh = m_dspController->devices() ? m_dspController->devices()->captureConfig.channels : 2;
        size_t pbCh = m_dspController->devices() ? m_dspController->devices()->playbackConfig.channels : 2;
        levelState.reset(capCh, pbCh);
        if (m_spectrumEngine)
            m_spectrumEngine->reset();
        if (m_spectrogramEngine)
            m_spectrogramEngine->reset();
        if (m_vectorScopeEngine)
            m_vectorScopeEngine->reset();
        emit levelsUpdated();
        return;
    }

    // Poll VU Levels
    if (levelState.visibilityCount > 0) {
        VuLevels levels = m_engine->getVuLevels();
        levelState.update(levels);
        emit levelsUpdated();
    } else {
        size_t capCh = m_dspController->devices() ? m_dspController->devices()->captureConfig.channels : 2;
        size_t pbCh = m_dspController->devices() ? m_dspController->devices()->playbackConfig.channels : 2;
        levelState.reset(capCh, pbCh);
    }

    // Poll Spectrum Engine
    if (m_spectrumEngine) {
        if (m_spectrumEngine->visibilityCount > 0) {
            SpectrumData specData;
            int specCh = m_spectrumEngine->channel.value_or(-1);
            if (m_engine->getSpectrum(m_spectrumEngine->isCapture, specCh, m_spectrumEngine->minFreq,
                                      m_spectrumEngine->maxFreq, m_spectrumEngine->nBins, specData)) {
                m_spectrumEngine->update(specData);
            } else {
                m_spectrumEngine->reset();
            }
        } else {
            m_spectrumEngine->reset();
        }
    }

    // Poll Spectrogram Engine
    if (m_spectrogramEngine) {
        if (m_spectrogramEngine->visibilityCount > 0) {
            SpectrumData spectroData;
            int spectroCh = m_spectrogramEngine->channel.value_or(-1);
            if (m_engine->getSpectrum(m_spectrogramEngine->isCapture, spectroCh, m_spectrogramEngine->minFreq,
                                      m_spectrogramEngine->maxFreq, m_spectrogramEngine->nBins, spectroData)) {
                m_spectrogramEngine->pushSpectrum(spectroData);
            } else {
                m_spectrogramEngine->reset();
            }
        } else {
            m_spectrogramEngine->reset();
        }
    }

    // Poll Vector Scope Engine
    if (m_vectorScopeEngine) {
        if (m_vectorScopeEngine->visibilityCount > 0) {
            AudioSamplesData samples;
            if (m_engine->getSamples(m_vectorScopeEngine->isCapture, m_vectorScopeEngine->nFrames, samples)) {
                m_vectorScopeEngine->update(samples);
            } else {
                m_vectorScopeEngine->reset();
            }
        } else {
            m_vectorScopeEngine->reset();
        }
    }
}
