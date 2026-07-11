#ifndef LEVEL_STATE_H
#define LEVEL_STATE_H

#include "config/DSPConfigTypes.h"
#include <vector>

struct LevelState {
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
};

#endif // LEVEL_STATE_H
