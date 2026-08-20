#ifndef SPECTROGRAM_ENGINE_H
#define SPECTROGRAM_ENGINE_H

#include "config/DSPConfigTypes.h"

#include <QObject>
#include <QSettings>
#include <deque>
#include <optional>
#include <vector>

enum class ColorPalette { Classic, Default = Classic, Viridis, Magma, Plasma, Inferno, Jet };

class SpectrogramEngine : public QObject {
    Q_OBJECT

public:
    explicit SpectrogramEngine(QObject* parent = nullptr) : QObject(parent) {
        loadSettings();
    }

    int visibilityCount = 0;
    bool isCapture = true;
    std::optional<int> channel = std::nullopt;
    size_t nBins = 200;
    double minFreq = 20.0;
    double maxFreq = 20000.0;
    bool show3D = false;
    ColorPalette colorPalette = ColorPalette::Classic;

    std::deque<SpectrumData> history;
    size_t maxHistory = 300;
    double timeWindow = 10.0;

    void loadSettings() {
        QSettings s("DSPMonitor", "MonitorQt");
        isCapture = s.value("spectrogram_is_capture", true).toBool();
        int ch = s.value("spectrogram_channel", -1).toInt();
        if (ch >= 0)
            channel = ch;
        else
            channel = std::nullopt;
        nBins = static_cast<size_t>(s.value("spectrogram_n_bins", 200).toInt());
        minFreq = s.value("spectrogram_min_freq", 20.0).toDouble();
        maxFreq = s.value("spectrogram_max_freq", 20000.0).toDouble();
        show3D = s.value("spectrogram_show_3d", false).toBool();
        colorPalette = static_cast<ColorPalette>(
            s.value("spectrogram_color_palette", static_cast<int>(ColorPalette::Classic)).toInt());
        timeWindow = s.value("spectrogram_time_window", 10.0).toDouble();
    }

    void saveSettings() const {
        QSettings s("DSPMonitor", "MonitorQt");
        s.setValue("spectrogram_is_capture", isCapture);
        s.setValue("spectrogram_channel", channel.has_value() ? channel.value() : -1);
        s.setValue("spectrogram_n_bins", static_cast<int>(nBins));
        s.setValue("spectrogram_min_freq", minFreq);
        s.setValue("spectrogram_max_freq", maxFreq);
        s.setValue("spectrogram_show_3d", show3D);
        s.setValue("spectrogram_color_palette", static_cast<int>(colorPalette));
        s.setValue("spectrogram_time_window", timeWindow);
    }

    void pushSpectrum(SpectrumData newData) {
        newData.timestamp = QDateTime::currentDateTime();
        history.push_back(newData);
        if (history.size() > maxHistory) {
            history.pop_front();
        }
        emit updated();
    }

    bool reset() {
        if (history.empty())
            return false;
        history.clear();
        emit updated();
        return true;
    }

    void resetToDefaults() {
        isCapture = true;
        channel = std::nullopt;
        nBins = 200;
        minFreq = 20.0;
        maxFreq = 20000.0;
        show3D = false;
        colorPalette = ColorPalette::Classic;
        timeWindow = 10.0;
        history.clear();
        saveSettings();
        emit updated();
    }

signals:
    void updated();
};

#endif // SPECTROGRAM_ENGINE_H
