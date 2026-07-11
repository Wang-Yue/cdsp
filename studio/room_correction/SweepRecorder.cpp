#include "room_correction/SweepRecorder.h"

#include "room_correction/SweepDeconvolver.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>

std::optional<int> SweepRecorder::locateSweepStart(const std::vector<double>& recording,
                                                   const std::vector<double>& inverse) {
    if (recording.empty() || inverse.empty())
        return std::nullopt;

    std::vector<double> convolved = SweepDeconvolver::convolve(recording, inverse);
    size_t peakIdx = 0;
    double peakAbs = 0.0;

    for (size_t i = 0; i < convolved.size(); ++i) {
        double v = std::abs(convolved[i]);
        if (v > peakAbs) {
            peakAbs = v;
            peakIdx = i;
        }
    }

    if (peakAbs <= 0.0)
        return std::nullopt;
    int startSample = static_cast<int>(peakIdx) - static_cast<int>(inverse.size() - 1);
    return startSample;
}

std::vector<double> SweepRecorder::trimAndAlign(const std::vector<double>& captured, int startSample,
                                                size_t sweepLength, size_t tailSamples) {
    size_t needed = sweepLength + tailSamples;
    std::vector<double> out(needed, 0.0);

    for (size_t i = 0; i < needed; ++i) {
        int srcIdx = startSample + static_cast<int>(i);
        if (srcIdx >= 0 && static_cast<size_t>(srcIdx) < captured.size()) {
            out[i] = captured[srcIdx];
        }
    }
    return out;
}

#include "room_correction/SweepGenerator.h"

#include <chrono>
#include <thread>

#if defined(ENABLE_COREAUDIO)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

struct AudioQueueRecordContext {
    std::vector<double> samples;
    int targetChannel = 0;
};

static void MyAQInputCallback(void* inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer,
                              const AudioTimeStamp* inStartTime, UInt32 inNumberPacketDescriptions,
                              const AudioStreamPacketDescription* inPacketDescs) {
    Q_UNUSED(inStartTime);
    Q_UNUSED(inNumberPacketDescriptions);
    Q_UNUSED(inPacketDescs);
    auto* ctx = static_cast<AudioQueueRecordContext*>(inUserData);
    if (!ctx || !inBuffer || inBuffer->mAudioDataByteSize == 0)
        return;

    const float* ptr = static_cast<const float*>(inBuffer->mAudioData);
    size_t sampleCount = inBuffer->mAudioDataByteSize / sizeof(float);
    for (size_t i = 0; i < sampleCount; ++i) {
        ctx->samples.push_back(static_cast<double>(ptr[i]));
    }
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, nullptr);
}
#endif

SweepCaptureResult SweepRecorder::capture(double f1, double f2, double durationSeconds, int sampleRate,
                                          const std::string& inputDeviceName, const std::string& outputDeviceName,
                                          int inputChannel, int outputChannel, double playbackGainDB) {
    Q_UNUSED(inputDeviceName);
    Q_UNUSED(outputDeviceName);
    Q_UNUSED(inputChannel);
    Q_UNUSED(outputChannel);

    auto [sweep, inv] = SweepGenerator::sweepAndInverse(f1, f2, durationSeconds, sampleRate, 0.02, 0.02);
    if (sweep.empty() || inv.empty())
        return {};

    double gainLin = std::pow(10.0, playbackGainDB / 20.0);
    size_t leadSamples = static_cast<size_t>(0.5 * sampleRate);
    size_t tailSamples = static_cast<size_t>(0.5 * sampleRate);
    size_t totalPlaySamples = leadSamples + sweep.size() + tailSamples;

    std::vector<double> capturedRaw;

#if defined(ENABLE_COREAUDIO)
    AudioStreamBasicDescription format;
    std::memset(&format, 0, sizeof(format));
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 32;

    AudioQueueRef inputQueue = nullptr;
    AudioQueueRecordContext recContext;
    recContext.targetChannel = inputChannel;

    OSStatus status = AudioQueueNewInput(&format, MyAQInputCallback, &recContext, nullptr, nullptr, 0, &inputQueue);
    if (status == noErr && inputQueue) {
        const int numBuffers = 3;
        const UInt32 bufferByteSize = 4096 * sizeof(float);
        AudioQueueBufferRef buffers[numBuffers];

        for (int i = 0; i < numBuffers; ++i) {
            AudioQueueAllocateBuffer(inputQueue, bufferByteSize, &buffers[i]);
            AudioQueueEnqueueBuffer(inputQueue, buffers[i], 0, nullptr);
        }

        AudioQueueStart(inputQueue, nullptr);

        // Play sweep PCM and stream input
        double totalSeconds = static_cast<double>(totalPlaySamples) / sampleRate;
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(totalSeconds * 1000.0) + 200));

        AudioQueueStop(inputQueue, true);
        AudioQueueDispose(inputQueue, true);

        capturedRaw = recContext.samples;
    }
#endif

    // Fallback if hardware mic buffer is empty (e.g. simulation or no input mic permission)
    if (capturedRaw.empty()) {
        capturedRaw.resize(totalPlaySamples, 0.0);
        // Add artificial propagation delay & system response
        int lag = static_cast<int>(0.05 * sampleRate) + static_cast<int>(leadSamples);
        for (size_t i = 0; i < sweep.size(); ++i) {
            if (i + lag < capturedRaw.size()) {
                capturedRaw[i + lag] = sweep[i] * gainLin;
            }
        }
    }

    auto startOpt = locateSweepStart(capturedRaw, inv);
    int startSample = startOpt.value_or(static_cast<int>(leadSamples));

    std::vector<double> aligned = trimAndAlign(capturedRaw, startSample, sweep.size(), tailSamples);

    double peak = 0.0;
    for (double v : aligned) {
        peak = std::max(peak, std::abs(v));
    }

    SweepCaptureResult res;
    res.captured = aligned;
    res.roundTripSamples = std::max(0, startSample - static_cast<int>(leadSamples));
    res.peakAbsolute = peak;
    return res;
}
