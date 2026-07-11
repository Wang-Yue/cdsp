#include "room_correction/CalibrationCurve.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>

CalibrationCurve::CalibrationCurve(const std::vector<double>& freqs, const std::vector<double>& mags, const std::vector<double>& phases)
    : frequencies(freqs), magnitudesDB(mags), phasesDeg(phases) {}

double CalibrationCurve::magnitude(double freqHz) const {
    if (frequencies.empty()) return 0.0;
    if (freqHz <= frequencies.front()) return magnitudesDB.front();
    if (freqHz >= frequencies.back()) return magnitudesDB.back();

    for (size_t i = 0; i < frequencies.size() - 1; ++i) {
        if (freqHz >= frequencies[i] && freqHz <= frequencies[i + 1]) {
            double logF = std::log10(freqHz);
            double logLo = std::log10(frequencies[i]);
            double logHi = std::log10(frequencies[i + 1]);
            double t = (logF - logLo) / (logHi - logLo);
            return magnitudesDB[i] + t * (magnitudesDB[i + 1] - magnitudesDB[i]);
        }
    }
    return magnitudesDB.back();
}

double CalibrationCurve::phase(double freqHz) const {
    if (phasesDeg.empty()) return 0.0;
    if (freqHz <= frequencies.front()) return phasesDeg.front();
    if (freqHz >= frequencies.back()) return phasesDeg.back();

    for (size_t i = 0; i < frequencies.size() - 1; ++i) {
        if (freqHz >= frequencies[i] && freqHz <= frequencies[i + 1]) {
            double logF = std::log10(freqHz);
            double logLo = std::log10(frequencies[i]);
            double logHi = std::log10(frequencies[i + 1]);
            double t = (logF - logLo) / (logHi - logLo);
            return phasesDeg[i] + t * (phasesDeg[i + 1] - phasesDeg[i]);
        }
    }
    return phasesDeg.back();
}

std::optional<CalibrationCurve> CalibrationCurve::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    std::vector<double> freqs, mags, phases;
    std::string line;

    while (std::getline(file, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        if (line[first] == '*' || line[first] == '#' || line[first] == '/') continue;

        std::stringstream ss(line.substr(first));
        double f, m, p;
        if (ss >> f >> m) {
            freqs.push_back(f);
            mags.push_back(m);
            if (ss >> p) phases.push_back(p);
        }
    }

    if (freqs.empty()) return std::nullopt;
    return CalibrationCurve(freqs, mags, phases);
}

bool CalibrationCurve::writeFRD(const std::string& path, const std::string& comment) const {
    std::ofstream file(path);
    if (!file.is_open()) return false;

    if (!comment.empty()) {
        std::stringstream ss(comment);
        std::string line;
        while (std::getline(ss, line)) {
            file << "* " << line << "\n";
        }
    }

    file << "* Freq(Hz) dB Phase(deg)\n";
    for (size_t i = 0; i < frequencies.size(); ++i) {
        file << frequencies[i] << "\t" << magnitudesDB[i];
        if (i < phasesDeg.size()) {
            file << "\t" << phasesDeg[i];
        } else {
            file << "\t0.0";
        }
        file << "\n";
    }

    return file.good();
}
