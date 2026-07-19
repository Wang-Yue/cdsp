#ifndef LEVEL_STATE_H
#define LEVEL_STATE_H

#include "config/DSPConfigTypes.h"

#include <mutex>
#include <vector>

struct LevelState {
    int visibilityCount = 0;
    int captureChannelCount = 0;
    int playbackChannelCount = 0;
    mutable std::mutex mutex;
    std::vector<float> captureRms;
    std::vector<float> capturePeak;
    std::vector<float> playbackRms;
    std::vector<float> playbackPeak;

    void update(const std::vector<float>& capPeak, const std::vector<float>& capRms, const std::vector<float>& pbPeak,
                const std::vector<float>& pbRms) {
        std::lock_guard<std::mutex> lock(mutex);
        capturePeak = capPeak;
        captureRms = capRms;
        playbackPeak = pbPeak;
        playbackRms = pbRms;
        if (captureChannelCount != static_cast<int>(capturePeak.size())) {
            captureChannelCount = static_cast<int>(capturePeak.size());
        }
        if (playbackChannelCount != static_cast<int>(playbackPeak.size())) {
            playbackChannelCount = static_cast<int>(playbackPeak.size());
        }
    }

    void update(const VuLevels& levels) {
        update(levels.capture_peak, levels.capture_rms, levels.playback_peak, levels.playback_rms);
    }

    bool reset(size_t capChannels = 0, size_t pbChannels = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        captureChannelCount = static_cast<int>(capChannels);
        playbackChannelCount = static_cast<int>(pbChannels);
        std::vector<float> capSilent(capChannels, -100.0f);
        std::vector<float> playSilent(pbChannels, -100.0f);

        if (capturePeak != capSilent || captureRms != capSilent || playbackPeak != playSilent ||
            playbackRms != playSilent) {
            capturePeak = capSilent;
            captureRms = capSilent;
            playbackPeak = playSilent;
            playbackRms = playSilent;
            return true;
        }
        return false;
    }
};

#endif // LEVEL_STATE_H
