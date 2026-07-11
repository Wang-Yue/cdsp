#include "room_correction/CalibrationCurve.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

CalibrationCurve::CalibrationCurve(const std::vector<double>& freqs, const std::vector<double>& mags,
                                   const std::vector<double>& phases)
    : frequencies(freqs), magnitudesDB(mags), phasesDeg(phases) {
    if (frequencies.size() == magnitudesDB.size() && !frequencies.empty()) {
        bool isSorted = true;
        for (size_t i = 1; i < frequencies.size(); ++i) {
            if (frequencies[i] < frequencies[i - 1]) {
                isSorted = false;
                break;
            }
        }
        if (!isSorted) {
            struct Entry {
                double f, m, p;
            };
            std::vector<Entry> entries(frequencies.size());
            for (size_t i = 0; i < frequencies.size(); ++i) {
                double pVal = (i < phasesDeg.size()) ? phasesDeg[i] : 0.0;
                entries[i] = {frequencies[i], magnitudesDB[i], pVal};
            }
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.f < b.f; });
            for (size_t i = 0; i < entries.size(); ++i) {
                frequencies[i] = entries[i].f;
                magnitudesDB[i] = entries[i].m;
                if (i < phasesDeg.size()) {
                    phasesDeg[i] = entries[i].p;
                }
            }
        }
    }
}

double CalibrationCurve::magnitude(double freqHz) const {
    if (frequencies.empty())
        return 0.0;
    if (freqHz <= frequencies.front())
        return magnitudesDB.front();
    if (freqHz >= frequencies.back())
        return magnitudesDB.back();

    auto it = std::lower_bound(frequencies.begin(), frequencies.end(), freqHz);
    size_t hi = std::distance(frequencies.begin(), it);
    size_t lo = (hi > 0) ? (hi - 1) : 0;

    double logF = std::log10(freqHz);
    double logLo = std::log10(frequencies[lo]);
    double logHi = std::log10(frequencies[hi]);
    if (logHi <= logLo)
        return magnitudesDB[lo];

    double t = (logF - logLo) / (logHi - logLo);
    return magnitudesDB[lo] + t * (magnitudesDB[hi] - magnitudesDB[lo]);
}

double CalibrationCurve::phase(double freqHz) const {
    if (phasesDeg.empty() || phasesDeg.size() < frequencies.size())
        return 0.0;
    if (freqHz <= frequencies.front())
        return phasesDeg.front();
    if (freqHz >= frequencies.back())
        return phasesDeg.back();

    auto it = std::lower_bound(frequencies.begin(), frequencies.end(), freqHz);
    size_t hi = std::distance(frequencies.begin(), it);
    size_t lo = (hi > 0) ? (hi - 1) : 0;

    double logF = std::log10(freqHz);
    double logLo = std::log10(frequencies[lo]);
    double logHi = std::log10(frequencies[hi]);
    if (logHi <= logLo)
        return phasesDeg[lo];

    double t = (logF - logLo) / (logHi - logLo);
    return phasesDeg[lo] + t * (phasesDeg[hi] - phasesDeg[lo]);
}

std::optional<CalibrationCurve> CalibrationCurve::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return std::nullopt;

    std::vector<double> freqs, mags, phases;
    std::string line;
    bool sawPhase = false;

    while (std::getline(file, line)) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            continue;
        if (line[first] == '*' || line[first] == '#' || line[first] == ';' || line[first] == '/')
            continue;

        std::stringstream ss(line.substr(first));
        double f, m, p;
        if (ss >> f >> m) {
            freqs.push_back(f);
            mags.push_back(m);
            if (ss >> p) {
                phases.push_back(p);
                sawPhase = true;
            } else if (sawPhase) {
                phases.push_back(0.0);
            }
        }
    }

    if (freqs.empty())
        return std::nullopt;

    if (!sawPhase) {
        phases.clear();
    }

    return CalibrationCurve(freqs, mags, phases);
}

bool CalibrationCurve::writeFRD(const std::string& path, const std::string& comment) const {
    std::ofstream file(path);
    if (!file.is_open())
        return false;

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
