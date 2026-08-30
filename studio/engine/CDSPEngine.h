#ifndef CDSP_ENGINE_H
#define CDSP_ENGINE_H

#include "config/DSPConfigTypes.h" // for Fader, AudioDevice, AudioDeviceDescriptor, AudioSamplesData, SpectrumData

#include <functional> // for function
#include <optional>   // for optional
#include <stddef.h>   // for size_t
#include <string>     // for string
#include <vector>     // for vector

extern "C" {
#include "Public/cdsp_pub_types.h" // for cdsp_fader_t, dsp_engine_t
}

class CDSPEngine {
public:
    using LogCallback =
        std::function<void(const std::string& level, const std::string& label, const std::string& message)>;

    CDSPEngine();
    ~CDSPEngine();

    // Disable copy
    CDSPEngine(const CDSPEngine&) = delete;
    CDSPEngine& operator=(const CDSPEngine&) = delete;

    bool start(const std::string& configJson, std::string& errorMessage);
    bool setConfig(const std::string& configJson, std::string& errorMessage);
    void stop();
    void poll();

    void setFaderVolume(Fader fader, float db, bool instant = false);
    void setFaderMute(Fader fader, bool mute);
    float getFaderVolume(Fader fader) const;
    bool isFaderMuted(Fader fader) const;

    StateUpdate getStatus() const;
    VuLevels getVuLevels() const;

    bool getSpectrum(bool isCapture, int channel, double minFreq, double maxFreq, size_t nBins,
                     SpectrumData& outSpectrum) const;
    bool getSamples(bool isCapture, size_t nFrames, AudioSamplesData& outSamples) const;

    std::vector<AudioDevice> getAvailableDevices(const std::string& backend, bool input) const;
    std::optional<AudioDeviceDescriptor> getDeviceCapabilities(const std::string& backend, const std::string& device,
                                                               bool isCapture) const;

    void setLogLevel(const std::string& levelStr);
    static void setLogCallback(LogCallback callback);

private:
    dsp_engine_t* m_engine = nullptr;

    static cdsp_fader_t faderToCFader(Fader fader);
};

#endif // CDSP_ENGINE_H
