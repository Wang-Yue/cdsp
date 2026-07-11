#ifndef AUDIO_SETTINGS_H
#define AUDIO_SETTINGS_H

#include "config/DSPConfigTypes.h"
#include <QObject>
#include <QSettings>
#include <functional>

class AudioSettings : public QObject {
    Q_OBJECT

public:
    explicit AudioSettings(QObject* parent = nullptr);

    int chunkSize = 1024;
    bool enableRateAdjust = false;

    bool resamplerEnabled = false;
    ResamplerType resamplerType = ResamplerType::Synchronous;
    ResamplerProfile resamplerProfile = ResamplerProfile::Balanced;
    bool resamplerUseProfile = true;
    int resamplerSincLen = 256;
    int resamplerOversamplingFactor = 128;
    std::string resamplerWindow = "BlackmanHarris";
    double resamplerFCutoff = 0.95;
    ResamplerInterpolation resamplerInterpolation = ResamplerInterpolation::Cubic;
    SincInterpolation resamplerSincInterpolation = SincInterpolation::Cubic;
    AppleResamplerQuality resamplerAppleQuality = AppleResamplerQuality::Max;
    AppleResamplerComplexity resamplerAppleComplexity = AppleResamplerComplexity::Normal;

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
    int silenceTimeout = 0;
    int queuelimit = 4;
    bool stopOnRateChange = false;
    double rateMeasureInterval = 1.0;
    bool multithreaded = false;
    int workerThreads = 0;

    float getVolume(Fader fader) const;
    void setVolume(float db, Fader fader);
    bool getMuted(Fader fader) const;
    void setMuted(bool muted, Fader fader);

    void loadPreferences();
    void savePreferences();

    std::function<void()> onChanged;

signals:
    void changed();

private:
    void notifyChange();
};

#endif // AUDIO_SETTINGS_H
