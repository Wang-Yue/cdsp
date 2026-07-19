#include "models/DSPEngineController.h"

#include "models/LogManager.h"

DSPEngineController::DSPEngineController(std::shared_ptr<CDSPEngine> engine,
                                         std::shared_ptr<AudioDeviceManager> devices,
                                         std::shared_ptr<AudioSettings> settings,
                                         std::shared_ptr<PipelineStore> pipeline,
                                         std::shared_ptr<MonitoringController> monitoring,
                                         std::shared_ptr<LevelState> levels, QObject* parent)
    : QObject(parent), m_engine(engine), m_devices(devices), m_settings(settings), m_pipeline(pipeline),
      m_monitoring(monitoring), m_levels(levels) {

    m_applyConfigTimer.setSingleShot(true);
    connect(&m_applyConfigTimer, &QTimer::timeout, this, &DSPEngineController::applyConfigAsync);

    if (m_monitoring) {
        setMonitoringController(m_monitoring);
    }

    if (m_devices) {
        connect(m_devices.get(), &AudioDeviceManager::configChanged, this, &DSPEngineController::applyConfig);
    }
    if (m_pipeline) {
        connect(m_pipeline.get(), &PipelineStore::pipelineChanged, this, &DSPEngineController::applyConfig);
    }
    if (m_settings) {
        connect(m_settings.get(), &AudioSettings::changed, this, [this]() {
            if (m_devices) {
                m_devices->validateSampleRates();
            }
            applyConfig();
        });
    }
}

void DSPEngineController::setMonitoringController(std::shared_ptr<MonitoringController> monitoring) {
    m_monitoring = monitoring;
    if (m_monitoring) {
        m_monitoring->onStatusChange = [this](ProcessingState newStatus) {
            if (newStatus != status) {
                status = newStatus;
                emit statusChanged(status);
            }
        };
        m_monitoring->onRestartEngine = [this]() { startEngine(); };
    }
}

DSPConfiguration DSPEngineController::buildConfiguration() const {
    DSPConfiguration config;

    int captureRate = m_devices ? m_devices->captureConfig.sampleRate : 48000;
    int playbackRate = m_devices ? m_devices->playbackConfig.sampleRate : 48000;
    int channelCount = m_devices ? m_devices->captureConfig.channels : 2;

    config.devices.samplerate = playbackRate;
    config.devices.chunksize = m_settings ? m_settings->chunkSize : 1024;
    if (m_settings && m_settings->enableRateAdjust) {
        config.devices.enableRateAdjust = true;
    }

    if (m_settings) {
        config.devices.queuelimit = m_settings->queuelimit;
        config.devices.stopOnRateChange = m_settings->stopOnRateChange;
        config.devices.rateMeasureInterval = m_settings->rateMeasureInterval;
        config.devices.multithreaded = m_settings->multithreaded;
        if (m_settings->multithreaded && m_settings->workerThreads > 0)
            config.devices.workerThreads = m_settings->workerThreads;
        if (m_settings->silenceTimeout > 0) {
            config.devices.silenceThreshold = static_cast<double>(m_settings->silenceThreshold);
            config.devices.silenceTimeout = static_cast<double>(m_settings->silenceTimeout);
        }

        if (m_settings->resamplerEnabled) {
            config.devices.captureSamplerate = captureRate;
            ResamplerConfig resCfg;

            ResamplerType effectiveType = m_settings->resamplerType;

            resCfg.type = effectiveType;
            switch (effectiveType) {
            case ResamplerType::AsyncSinc:
                if (m_settings->resamplerUseProfile) {
                    resCfg.profile = resamplerProfileToString(m_settings->resamplerProfile);
                } else {
                    resCfg.sincLen = m_settings->resamplerSincLen;
                    resCfg.oversamplingFactor = m_settings->resamplerOversamplingFactor;
                    resCfg.window = m_settings->resamplerWindow;
                    resCfg.fCutoff = m_settings->resamplerFCutoff;
                    resCfg.interpolation = sincInterpolationToString(m_settings->resamplerSincInterpolation);
                }
                break;
            case ResamplerType::AsyncPoly:
                resCfg.interpolation = resamplerInterpolationToString(m_settings->resamplerInterpolation);
                break;
            case ResamplerType::Synchronous:
            case ResamplerType::Slip:
                break;
            }
            config.devices.resampler = resCfg;
        }
    }

    if (m_devices) {
        config.devices.capture = m_devices->captureConfig.toCaptureDeviceConfig();
        config.devices.playback = m_devices->playbackConfig.toPlaybackDeviceConfig();

#if defined(ENABLE_COREAUDIO)
        if (config.devices.playback.backend == AudioBackendType::CoreAudio) {
            config.devices.playback.coreAudio.exclusive = m_devices->exclusiveMode;
        }
#endif
#if defined(ENABLE_WASAPI)
        if (config.devices.playback.backend == AudioBackendType::WASAPI) {
            if (m_devices->exclusiveMode) {
                config.devices.playback.wasapi.exclusive = true;
            }
        }
#endif

        if (m_devices->playbackConfig.backend == AudioBackendType::SignalGenerator) {
            PlaybackDeviceConfig pb;
#if defined(ENABLE_COREAUDIO)
            pb.backend = AudioBackendType::CoreAudio;
            pb.coreAudio.channels = m_devices->playbackConfig.channels;
            pb.coreAudio.device = m_devices->playbackConfig.deviceName();
            pb.coreAudio.exclusive = m_devices->exclusiveMode;
#elif defined(ENABLE_WASAPI)
            pb.backend = AudioBackendType::WASAPI;
            pb.wasapi.channels = m_devices->playbackConfig.channels;
            pb.wasapi.device = m_devices->playbackConfig.deviceName();
            if (m_devices->exclusiveMode)
                pb.wasapi.exclusive = true;
#elif defined(ENABLE_ALSA)
            pb.backend = AudioBackendType::ALSA;
            pb.alsa.channels = m_devices->playbackConfig.channels;
            pb.alsa.device = m_devices->playbackConfig.deviceName();
#else
            pb.backend = AudioBackendType::RawFile;
            pb.rawFile.channels = m_devices->playbackConfig.channels;
            pb.rawFile.filename = m_devices->playbackConfig.filename;
#endif
            config.devices.playback = pb;
        }
    }

    if (m_pipeline) {
        auto buildRes = m_pipeline->buildPipeline(captureRate, channelCount);
        config.filters = buildRes.filters;
        config.mixers = buildRes.mixers;
        config.processors = buildRes.processors;
        config.pipeline = buildRes.steps;
    }

    return config;
}

void DSPEngineController::startEngine() {
    runApplyConfigTask();
}

void DSPEngineController::stopEngine() {
    m_applyConfigTimer.stop();
    if (m_levels && m_devices) {
        m_levels->reset(m_devices->captureConfig.channels, m_devices->playbackConfig.channels);
    }
    if (m_engine) {
        m_engine->stop();
    }
    status = ProcessingState::Inactive;
    emit statusChanged(status);
    LogManager::instance()->appendLog(LogLevel::Info, "DSP Engine stopped.");
}

void DSPEngineController::applyConfig() {
    if (status == ProcessingState::Inactive)
        return;
    runApplyConfigTask();
}

void DSPEngineController::runApplyConfigTask() {
    if (m_devices && !m_devices->devicesAvailable())
        return;
    m_applyConfigTimer.stop();
    m_applyConfigTimer.start(10); // 10ms debounce delay matching SwiftUI Task.sleep(nanoseconds: 10_000_000)
}

void DSPEngineController::applyConfigAsync() {
    if (m_pipeline) {
        m_pipeline->save();
    }

    syncFaders();

    DSPConfiguration config = buildConfiguration();
    std::string jsonStr = config.toJsonString();

    std::string err;
    bool ok = m_engine->start(jsonStr, err);
    if (ok) {
        emit configApplied();
        LogManager::instance()->appendLog(LogLevel::Info, "DSP Engine configuration applied successfully.");
    } else {
        lastErrorMessage = err;
        LogManager::instance()->appendLog(LogLevel::Error, QString::fromStdString("Apply config failed: " + err));
    }
}

void DSPEngineController::setFaderVolume(Fader fader, float db, bool instant) {
    if (m_settings) {
        m_settings->setVolume(db, fader);
    }
    if (m_engine) {
        m_engine->setFaderVolume(fader, db, instant);
    }
}

void DSPEngineController::setFaderMute(Fader fader, bool mute) {
    if (m_settings) {
        m_settings->setMuted(mute, fader);
    }
    if (m_engine) {
        m_engine->setFaderMute(fader, mute);
    }
}

void DSPEngineController::syncFaders() {
    if (!m_settings || !m_engine)
        return;
    for (Fader f : {Fader::Main, Fader::Aux1, Fader::Aux2, Fader::Aux3, Fader::Aux4}) {
        m_engine->setFaderMute(f, m_settings->getMuted(f));
        m_engine->setFaderVolume(f, m_settings->getVolume(f));
    }
}

void DSPEngineController::toggleFaderMute(Fader fader) {
    if (!m_settings)
        return;
    bool currentMute = m_settings->getMuted(fader);
    setFaderMute(fader, !currentMute);
}
