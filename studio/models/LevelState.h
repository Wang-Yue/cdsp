#ifndef LEVEL_STATE_H
#define LEVEL_STATE_H

#include "config/DSPConfigTypes.h"
#include <vector>

struct LevelState {
    int visibilityCount = 0;
    std::vector<float> captureRms;
    std::vector<float> capturePeak;
    std::vector<float> playbackRms;
    std::vector<float> playbackPeak;

    void update(const VuLevels& levels) {
        captureRms = levels.capture_rms;
        capturePeak = levels.capture_peak;
        playbackRms = levels.playback_rms;
        playbackPeak = levels.playback_peak;
    }

    void reset(size_t capChannels = 0, size_t pbChannels = 0) {
        captureRms.assign(capChannels, -100.0f);
        capturePeak.assign(capChannels, -100.0f);
        playbackRms.assign(pbChannels, -100.0f);
        playbackPeak.assign(pbChannels, -100.0f);
    }
};

#endif // LEVEL_STATE_H
