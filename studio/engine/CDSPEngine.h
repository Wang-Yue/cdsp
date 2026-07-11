#ifndef CDSP_ENGINE_H
#define CDSP_ENGINE_H

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif
#include <complex>
#include <bitset>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <optional>

#include "config/DSPConfigTypes.h"

extern "C" {
#include "Engine/dsp_engine.h"
}

class CDSPEngine {
public:
    CDSPEngine();
    ~CDSPEngine();

    // Disable copy
    CDSPEngine(const CDSPEngine&) = delete;
    CDSPEngine& operator=(const CDSPEngine&) = delete;

    bool start(const std::string& configJson, std::string& errorMessage);
    void stop();

    void setFaderVolume(Fader fader, float db, bool instant = false);
    void setFaderMute(Fader fader, bool mute);
    float getFaderVolume(Fader fader) const;
    bool isFaderMuted(Fader fader) const;

    StateUpdate getStatus() const;
    VuLevels getVuLevels() const;

    bool getSpectrum(bool isCapture, int channel, double minFreq, double maxFreq, size_t nBins, SpectrumData& outSpectrum) const;
    bool getSamples(bool isCapture, size_t nFrames, AudioSamplesData& outSamples) const;

    std::vector<AudioDevice> getAvailableDevices(const std::string& backend, bool input) const;
    std::optional<AudioDeviceDescriptor> getDeviceCapabilities(const std::string& backend, const std::string& device, bool isCapture) const;

    void setLogLevel(const std::string& levelStr);

private:
    dsp_engine_t* m_engine = nullptr;
    mutable std::mutex m_mutex;

    static fader_t faderToCFader(Fader fader);
};

#endif // CDSP_ENGINE_H
