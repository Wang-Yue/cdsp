#ifndef SPECTRUM_ENGINE_H
#define SPECTRUM_ENGINE_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <QSettings>
#include <optional>
#include <vector>

enum class FFTWindowFunction { Hann, Hamming, Blackman, FlatTop, Rectangular };
enum class OctaveSmoothing { None, OneThird, OneSixth, OneTwelfth, OneTwentyFourth };

class SpectrumEngine : public QObject {
    Q_OBJECT

public:
    explicit SpectrumEngine(QObject* parent = nullptr) : QObject(parent) { loadSettings(); }

    int visibilityCount = 0;
    bool isCapture = true;
    std::optional<int> channel = std::nullopt;
    size_t nBins = 30;
    double minFreq = 25.0;
    double maxFreq = 20000.0;
    double minDB = -120.0;
    double maxDB = 0.0;
    FFTWindowFunction windowFunction = FFTWindowFunction::Hann;
    OctaveSmoothing smoothing = OctaveSmoothing::None;
    float peakHoldDecayRate = 0.95f;

    SpectrumData data;

    void loadSettings() {
        QSettings s("DSPMonitor", "MonitorQt");
        isCapture = s.value("spectrum_is_capture", true).toBool();
        int ch = s.value("spectrum_channel", -1).toInt();
        if (ch >= 0)
            channel = ch;
        else
            channel = std::nullopt;
        nBins = static_cast<size_t>(s.value("spectrum_n_bins", 30).toInt());
        minFreq = s.value("spectrum_min_freq", 25.0).toDouble();
        maxFreq = s.value("spectrum_max_freq", 20000.0).toDouble();
        minDB = s.value("spectrum_min_db", -120.0).toDouble();
        maxDB = s.value("spectrum_max_db", 0.0).toDouble();
        windowFunction = static_cast<FFTWindowFunction>(
            s.value("spectrum_window_fn", static_cast<int>(FFTWindowFunction::Hann)).toInt());
        smoothing = static_cast<OctaveSmoothing>(
            s.value("spectrum_smoothing", static_cast<int>(OctaveSmoothing::None)).toInt());
        peakHoldDecayRate = s.value("spectrum_peak_hold_decay", 0.95f).toFloat();
    }

    void saveSettings() const {
        QSettings s("DSPMonitor", "MonitorQt");
        s.setValue("spectrum_is_capture", isCapture);
        s.setValue("spectrum_channel", channel.has_value() ? channel.value() : -1);
        s.setValue("spectrum_n_bins", static_cast<int>(nBins));
        s.setValue("spectrum_min_freq", minFreq);
        s.setValue("spectrum_max_freq", maxFreq);
        s.setValue("spectrum_min_db", minDB);
        s.setValue("spectrum_max_db", maxDB);
        s.setValue("spectrum_window_fn", static_cast<int>(windowFunction));
        s.setValue("spectrum_smoothing", static_cast<int>(smoothing));
        s.setValue("spectrum_peak_hold_decay", peakHoldDecayRate);
    }

    void update(const SpectrumData& newData) {
        data = newData;
        emit updated();
    }

    bool reset() {
        if (data.magnitudes.empty() && data.frequencies.empty())
            return false;
        data = SpectrumData();
        emit updated();
        return true;
    }

    void resetToDefaults() {
        isCapture = true;
        channel = std::nullopt;
        nBins = 30;
        minFreq = 25.0;
        maxFreq = 20000.0;
        minDB = -120.0;
        maxDB = 0.0;
        windowFunction = FFTWindowFunction::Hann;
        smoothing = OctaveSmoothing::None;
        peakHoldDecayRate = 0.95f;
        data = SpectrumData();
        saveSettings();
        emit updated();
    }

signals:
    void updated();
};

#endif // SPECTRUM_ENGINE_H
