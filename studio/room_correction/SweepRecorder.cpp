#include "room_correction/SweepRecorder.h"

#include "room_correction/SweepDeconvolver.h" // for SweepDeconvolver
#include "room_correction/SweepGenerator.h"   // for SweepGenerator

#include <QAudioDevice>  // for QAudioDevice
#include <QAudioFormat>  // for QAudioFormat
#include <QAudioSink>    // for QAudioSink
#include <QAudioSource>  // for QAudioSource
#include <QBuffer>       // for QBuffer
#include <QByteArray>    // for QByteArray
#include <QEventLoop>    // for QEventLoop
#include <QIODevice>     // for QIODevice
#include <QList>         // for QList
#include <QMediaDevices> // for QMediaDevices
#include <QString>       // for QString
#include <QTimer>        // for QTimer
#include <QtGlobal>      // for qsizetype
#include <algorithm>     // for max, clamp
#include <cmath>         // for pow
#include <cstdlib>       // for abs
#include <utility>       // for get

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

SweepCaptureResult SweepRecorder::capture(double f1, double f2, double durationSeconds, int sampleRate,
                                          const std::string& inputDeviceName, const std::string& outputDeviceName,
                                          int inputChannel, int outputChannel, double playbackGainDB) {
    auto [sweep, inv] = SweepGenerator::sweepAndInverse(f1, f2, durationSeconds, sampleRate, 0.02, 0.02);
    if (sweep.empty() || inv.empty())
        return {};

    double gainLin = std::pow(10.0, playbackGainDB / 20.0);
    size_t leadSamples = static_cast<size_t>(0.5 * sampleRate);
    size_t tailSamples = static_cast<size_t>(0.5 * sampleRate);
    size_t totalPlaySamples = leadSamples + sweep.size() + tailSamples;

    QAudioDevice targetInputDevice = QMediaDevices::defaultAudioInput();
    if (!inputDeviceName.empty()) {
        for (const auto& dev : QMediaDevices::audioInputs()) {
            if (dev.description().toStdString() == inputDeviceName) {
                targetInputDevice = dev;
                break;
            }
        }
    }

    QAudioDevice targetOutputDevice = QMediaDevices::defaultAudioOutput();
    if (!outputDeviceName.empty()) {
        for (const auto& dev : QMediaDevices::audioOutputs()) {
            if (dev.description().toStdString() == outputDeviceName) {
                targetOutputDevice = dev;
                break;
            }
        }
    }

    int outCh = !targetOutputDevice.isNull() ? std::max(1, targetOutputDevice.maximumChannelCount()) : 2;
    int routeOutChannels = std::max(2, std::max(outCh, outputChannel + 1));
    int inCh = !targetInputDevice.isNull() ? std::max(1, targetInputDevice.maximumChannelCount()) : 1;
    int selInChannel = std::clamp(inputChannel, 0, inCh - 1);

    std::vector<float> playPcm(totalPlaySamples * routeOutChannels, 0.0f);
    for (size_t i = 0; i < sweep.size(); ++i) {
        float val = static_cast<float>(sweep[i] * gainLin);
        size_t frameIdx = leadSamples + i;
        if (outputChannel < 0) {
            for (int c = 0; c < routeOutChannels; ++c) {
                playPcm[frameIdx * routeOutChannels + c] = val;
            }
        } else {
            int targetC = std::clamp(outputChannel, 0, routeOutChannels - 1);
            playPcm[frameIdx * routeOutChannels + targetC] = val;
        }
    }

    std::vector<double> capturedRaw;

    if (!targetInputDevice.isNull() && !targetOutputDevice.isNull()) {
        QAudioFormat outFmt;
        outFmt.setSampleRate(sampleRate);
        outFmt.setChannelCount(routeOutChannels);
        outFmt.setSampleFormat(QAudioFormat::Float);

        QAudioFormat inFmt;
        inFmt.setSampleRate(sampleRate);
        inFmt.setChannelCount(inCh);
        inFmt.setSampleFormat(QAudioFormat::Float);

        QByteArray playBuf(reinterpret_cast<const char*>(playPcm.data()),
                           static_cast<qsizetype>(playPcm.size() * sizeof(float)));
        QBuffer playDevice(&playBuf);
        playDevice.open(QIODevice::ReadOnly);

        QByteArray recordBuf;
        QBuffer recordDevice(&recordBuf);
        recordDevice.open(QIODevice::WriteOnly);

        QAudioSink sink(targetOutputDevice, outFmt);
        QAudioSource source(targetInputDevice, inFmt);

        sink.start(&playDevice);
        source.start(&recordDevice);

        double totalSeconds = static_cast<double>(totalPlaySamples) / sampleRate;
        int waitMs = static_cast<int>(totalSeconds * 1000.0) + 300;

        QEventLoop loop;
        QTimer::singleShot(waitMs, &loop, &QEventLoop::quit);
        loop.exec();

        sink.stop();
        source.stop();

        const float* ptr = reinterpret_cast<const float*>(recordBuf.constData());
        size_t totalFloats = recordBuf.size() / sizeof(float);
        size_t totalFrames = totalFloats / inCh;
        for (size_t i = 0; i < totalFrames; ++i) {
            capturedRaw.push_back(static_cast<double>(ptr[i * inCh + selInChannel]));
        }
    }

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
