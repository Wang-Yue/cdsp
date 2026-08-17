#ifndef AUDIO_SETTINGS_H
#define AUDIO_SETTINGS_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <QSettings>
#include <functional>

class AudioSettings : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY(AudioSettings)

public:
    explicit AudioSettings(QObject* parent = nullptr);

    DevicesConfig deviceConfig;
    int chunkSize = 1024;
    bool enableRateAdjust = false;

    bool resamplerEnabled = false;
    ResamplerType resamplerType = ResamplerType::Synchronous;
    ResamplerProfile resamplerProfile = ResamplerProfile::Balanced;
    bool resamplerUseProfile = true;
    double resamplerAttenuation = 0.0;
    int resamplerSincLen = 256;
    int resamplerOversamplingFactor = 128;
    std::string resamplerWindow = "BlackmanHarris";
    double resamplerFCutoff = 0.95;
    ResamplerInterpolation resamplerInterpolation = ResamplerInterpolation::Cubic;
    SincInterpolation resamplerSincInterpolation = SincInterpolation::Cubic;

    float volume = 0.0f;
    bool isMuted = false;

    float fader1Volume = 0.0f;
    float fader2Volume = 0.0f;
    float fader3Volume = 0.0f;
    float fader4Volume = 0.0f;

    bool fader1Muted = false;
    bool fader2Muted = false;
    bool fader3Muted = false;
    bool fader4Muted = false;

    int silenceThreshold = -60;
    void setSilenceThreshold(int val);
    double silenceThresholdDouble() const { return static_cast<double>(silenceThreshold); }
    void setSilenceThresholdDouble(double val);

    int silenceTimeout = 0;
    void setSilenceTimeout(int val);
    double silenceTimeoutDouble() const { return static_cast<double>(silenceTimeout); }
    void setSilenceTimeoutDouble(double val);

    int queuelimit = 4;
    bool stopOnRateChange = false;
    double rateMeasureInterval = 1.0;
    bool multithreaded = false;
    int workerThreads = 0;

    bool showLevelMetersInDashboard = true;
    bool showSpectrumInDashboard = true;
    bool showSpectrogramInDashboard = true;
    bool showVectorScopeInDashboard = true;
    bool showAnalogVUInDashboard = true;
    bool showSignalGraphInDashboard = true;
    bool autoStartEngine = false;
    int logLevel = 2; // Default to Info level
    bool closeToTray = true;
    bool minimizeToTray = false;

    float getVolume(Fader fader) const;
    void setVolume(float db, Fader fader);
    bool getMuted(Fader fader) const;
    void setMuted(bool muted, Fader fader);

    void loadPreferences();
    void savePreferences();

    std::function<void()> onChanged;

signals:
    void changed();
    void settingsChanged();

private:
    void notifyChange();
};

#endif // AUDIO_SETTINGS_H
