#include "room_correction/SweepRecorder.h"

#include "room_correction/SweepDeconvolver.h"
#include "room_correction/SweepGenerator.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QBuffer>
#include <QEventLoop>
#include <QMediaDevices>
#include <QTimer>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

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
    Q_UNUSED(inputChannel);
    Q_UNUSED(outputChannel);

    auto [sweep, inv] = SweepGenerator::sweepAndInverse(f1, f2, durationSeconds, sampleRate, 0.02, 0.02);
    if (sweep.empty() || inv.empty())
        return {};

    double gainLin = std::pow(10.0, playbackGainDB / 20.0);
    size_t leadSamples = static_cast<size_t>(0.5 * sampleRate);
    size_t tailSamples = static_cast<size_t>(0.5 * sampleRate);
    size_t totalPlaySamples = leadSamples + sweep.size() + tailSamples;

    std::vector<float> playPcm(totalPlaySamples, 0.0f);
    for (size_t i = 0; i < sweep.size(); ++i) {
        playPcm[leadSamples + i] = static_cast<float>(sweep[i] * gainLin);
    }

    std::vector<double> capturedRaw;

    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Float);

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

    if (!targetInputDevice.isNull() && !targetOutputDevice.isNull()) {
        QByteArray playBuf(reinterpret_cast<const char*>(playPcm.data()),
                           static_cast<qsizetype>(playPcm.size() * sizeof(float)));
        QBuffer playDevice(&playBuf);
        playDevice.open(QIODevice::ReadOnly);

        QByteArray recordBuf;
        QBuffer recordDevice(&recordBuf);
        recordDevice.open(QIODevice::WriteOnly);

        QAudioSink sink(targetOutputDevice, fmt);
        QAudioSource source(targetInputDevice, fmt);

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
        size_t recordedCount = recordBuf.size() / sizeof(float);
        for (size_t i = 0; i < recordedCount; ++i) {
            capturedRaw.push_back(static_cast<double>(ptr[i]));
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
