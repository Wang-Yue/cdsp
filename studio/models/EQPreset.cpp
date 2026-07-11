#include "models/EQPreset.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

std::string eqBandTypeToString(EQBandType type) {
    switch (type) {
    case EQBandType::Peaking: return "Peaking";
    case EQBandType::Lowshelf: return "Lowshelf";
    case EQBandType::Highshelf: return "Highshelf";
    case EQBandType::Lowpass: return "Lowpass";
    case EQBandType::Highpass: return "Highpass";
    case EQBandType::LowpassFO: return "LowpassFO";
    case EQBandType::HighpassFO: return "HighpassFO";
    case EQBandType::LowshelfFO: return "LowshelfFO";
    case EQBandType::HighshelfFO: return "HighshelfFO";
    case EQBandType::Notch: return "Notch";
    case EQBandType::Bandpass: return "Bandpass";
    case EQBandType::Allpass: return "Allpass";
    case EQBandType::AllpassFO: return "AllpassFO";
    case EQBandType::Free: return "Free";
    case EQBandType::GeneralNotch: return "GeneralNotch";
    case EQBandType::LinkwitzTransform: return "LinkwitzTransform";
    }
    return "Peaking";
}

EQBandType stringToEQBandType(const std::string& str) {
    if (str == "Lowshelf") return EQBandType::Lowshelf;
    if (str == "Highshelf") return EQBandType::Highshelf;
    if (str == "Lowpass") return EQBandType::Lowpass;
    if (str == "Highpass") return EQBandType::Highpass;
    if (str == "LowpassFO") return EQBandType::LowpassFO;
    if (str == "HighpassFO") return EQBandType::HighpassFO;
    if (str == "LowshelfFO") return EQBandType::LowshelfFO;
    if (str == "HighshelfFO") return EQBandType::HighshelfFO;
    if (str == "Notch") return EQBandType::Notch;
    if (str == "Bandpass") return EQBandType::Bandpass;
    if (str == "Allpass") return EQBandType::Allpass;
    if (str == "AllpassFO") return EQBandType::AllpassFO;
    if (str == "Free") return EQBandType::Free;
    if (str == "GeneralNotch") return EQBandType::GeneralNotch;
    if (str == "LinkwitzTransform") return EQBandType::LinkwitzTransform;
    return EQBandType::Peaking;
}

std::string eqBandTypeToShortName(EQBandType type) {
    switch (type) {
    case EQBandType::Peaking: return "PK";
    case EQBandType::Lowshelf: return "LS";
    case EQBandType::Highshelf: return "HS";
    case EQBandType::Lowpass: return "LP";
    case EQBandType::Highpass: return "HP";
    case EQBandType::Notch: return "NO";
    case EQBandType::Bandpass: return "BP";
    case EQBandType::Allpass: return "AP";
    default: return eqBandTypeToString(type);
    }
}

EQBandType shortNameToEQBandType(const std::string& s) {
    std::string str = s;
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    if (str == "PK") return EQBandType::Peaking;
    if (str == "LS" || str == "LSC") return EQBandType::Lowshelf;
    if (str == "HS" || str == "HSC") return EQBandType::Highshelf;
    if (str == "LP" || str == "LPC") return EQBandType::Lowpass;
    if (str == "HP" || str == "HPC") return EQBandType::Highpass;
    if (str == "NO") return EQBandType::Notch;
    if (str == "BP") return EQBandType::Bandpass;
    if (str == "AP" || str == "APO") return EQBandType::Allpass;
    return stringToEQBandType(s);
}

bool eqBandTypeHasGain(EQBandType type) {
    switch (type) {
    case EQBandType::Peaking:
    case EQBandType::Lowshelf:
    case EQBandType::Highshelf:
    case EQBandType::LowshelfFO:
    case EQBandType::HighshelfFO:
        return true;
    default:
        return false;
    }
}

bool eqBandTypeHasQ(EQBandType type) {
    switch (type) {
    case EQBandType::LowpassFO:
    case EQBandType::HighpassFO:
    case EQBandType::LowshelfFO:
    case EQBandType::HighshelfFO:
    case EQBandType::AllpassFO:
    case EQBandType::Free:
    case EQBandType::GeneralNotch:
    case EQBandType::LinkwitzTransform:
        return false;
    default:
        return true;
    }
}

EQBand::EQBand() : id(QUuid::createUuid()) {}

EQBand::EQBand(EQBandType type, double freq, double gain, double q, bool enabled)
    : id(QUuid::createUuid()), type(type), freq(freq), gain(gain), q(q), isEnabled(enabled) {}

std::optional<BiquadCoefficients> EQBand::coefficients(int sampleRate) const {
    auto biquadType = stringToBiquadType(eqBandTypeToString(type));
    if (!biquadType.has_value()) return std::nullopt;

    BiquadParameters params;
    params.type = biquadType.value();

    switch (type) {
    case EQBandType::Free:
        params.b0 = b0; params.b1 = b1; params.b2 = b2;
        params.a1 = a1; params.a2 = a2;
        break;
    case EQBandType::GeneralNotch:
        params.freqNotch = freqNotch;
        params.freqPole = freqPole;
        params.qP = qPole;
        params.normalizeAtDc = normalizeAtDc;
        break;
    case EQBandType::LinkwitzTransform:
        params.freqAct = freqAct;
        params.qAct = qAct;
        params.freqTarget = freqTarget;
        params.qTarget = qTarget;
        break;
    case EQBandType::Lowshelf:
    case EQBandType::Highshelf:
        params.freq = freq;
        params.gain = gain;
        if (useSlope) params.slope = slope;
        else params.q = q;
        break;
    case EQBandType::Notch:
    case EQBandType::Bandpass:
    case EQBandType::Allpass:
        params.freq = freq;
        if (useBandwidth) params.bandwidth = bandwidth;
        else params.q = q;
        break;
    default:
        params.freq = freq;
        if (eqBandTypeHasGain(type)) params.gain = gain;
        if (eqBandTypeHasQ(type)) params.q = q;
        break;
    }

    return BiquadCoefficients::compute(params, sampleRate);
}

double EQBand::response(double f, int sampleRate) const {
    if (!isEnabled) return 0.0;
    auto coeffs = coefficients(sampleRate);
    if (!coeffs.has_value()) return 0.0;
    return coeffs.value().gainDB(f, sampleRate);
}

double EQBand::phaseResponse(double f, int sampleRate) const {
    if (!isEnabled) return 0.0;
    auto coeffs = coefficients(sampleRate);
    if (!coeffs.has_value()) return 0.0;
    return coeffs.value().phaseRad(f, sampleRate);
}

QJsonObject EQBand::toJson() const {
    QJsonObject obj;
    obj["id"] = id.toString();
    obj["type"] = QString::fromStdString(eqBandTypeToString(type));
    obj["freq"] = freq;
    obj["gain"] = gain;
    obj["q"] = q;
    obj["isEnabled"] = isEnabled;
    obj["b0"] = b0; obj["b1"] = b1; obj["b2"] = b2;
    obj["a1"] = a1; obj["a2"] = a2;
    obj["freqNotch"] = freqNotch; obj["freqPole"] = freqPole;
    obj["qPole"] = qPole; obj["normalizeAtDc"] = normalizeAtDc;
    obj["slope"] = slope; obj["bandwidth"] = bandwidth;
    obj["useSlope"] = useSlope; obj["useBandwidth"] = useBandwidth;
    obj["freqAct"] = freqAct; obj["qAct"] = qAct;
    obj["freqTarget"] = freqTarget; obj["qTarget"] = qTarget;
    return obj;
}

EQBand EQBand::fromJson(const QJsonObject& json) {
    EQBand b;
    if (json.contains("id")) b.id = QUuid::fromString(json["id"].toString());
    if (json.contains("type")) b.type = stringToEQBandType(json["type"].toString().toStdString());
    if (json.contains("freq")) b.freq = json["freq"].toDouble();
    if (json.contains("gain")) b.gain = json["gain"].toDouble();
    if (json.contains("q")) b.q = json["q"].toDouble();
    if (json.contains("isEnabled")) b.isEnabled = json["isEnabled"].toBool();
    if (json.contains("b0")) b.b0 = json["b0"].toDouble();
    if (json.contains("b1")) b.b1 = json["b1"].toDouble();
    if (json.contains("b2")) b.b2 = json["b2"].toDouble();
    if (json.contains("a1")) b.a1 = json["a1"].toDouble();
    if (json.contains("a2")) b.a2 = json["a2"].toDouble();
    if (json.contains("freqNotch")) b.freqNotch = json["freqNotch"].toDouble();
    if (json.contains("freqPole")) b.freqPole = json["freqPole"].toDouble();
    if (json.contains("qPole")) b.qPole = json["qPole"].toDouble();
    if (json.contains("normalizeAtDc")) b.normalizeAtDc = json["normalizeAtDc"].toBool();
    if (json.contains("slope")) b.slope = json["slope"].toDouble();
    if (json.contains("bandwidth")) b.bandwidth = json["bandwidth"].toDouble();
    if (json.contains("useSlope")) b.useSlope = json["useSlope"].toBool();
    if (json.contains("useBandwidth")) b.useBandwidth = json["useBandwidth"].toBool();
    if (json.contains("freqAct")) b.freqAct = json["freqAct"].toDouble();
    if (json.contains("qAct")) b.qAct = json["qAct"].toDouble();
    if (json.contains("freqTarget")) b.freqTarget = json["freqTarget"].toDouble();
    if (json.contains("qTarget")) b.qTarget = json["qTarget"].toDouble();
    return b;
}

bool EQBand::operator==(const EQBand& other) const {
    return id == other.id && type == other.type && freq == other.freq && gain == other.gain && q == other.q && isEnabled == other.isEnabled;
}

EQPreset::EQPreset() : id(QUuid::createUuid()), name("New Preset") {}

EQPreset::EQPreset(const std::string& name, double preampGain, const std::vector<EQBand>& bands)
    : id(QUuid::createUuid()), name(name), preampGain(preampGain), bands(bands) {}

void EQPreset::addBand(const EQBand& band) {
    bands.push_back(band);
}

void EQPreset::removeBand(size_t index) {
    if (index < bands.size()) {
        bands.erase(bands.begin() + index);
    }
}

double EQPreset::combinedResponse(double f, int sampleRate) const {
    double res = preampGain;
    for (const auto& band : bands) {
        if (band.isEnabled) {
            res += band.response(f, sampleRate);
        }
    }
    return res;
}

double EQPreset::combinedPhase(double f, int sampleRate) const {
    double res = 0.0;
    for (const auto& band : bands) {
        if (band.isEnabled) {
            res += band.phaseResponse(f, sampleRate);
        }
    }
    return res;
}

std::string EQPreset::toCSV() const {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1);
    ss << "Preamp: " << preampGain << " dB\n";

    for (size_t i = 0; i < bands.size(); ++i) {
        const auto& band = bands[i];
        std::string state = band.isEnabled ? "ON" : "OFF";
        std::string shortType = eqBandTypeToShortName(band.type);

        ss << "Filter " << (i + 1) << ": " << state << " " << shortType << " ";
        if (band.type == EQBandType::Free) {
            ss << "B0 " << band.b0 << " B1 " << band.b1 << " B2 " << band.b2 << " A1 " << band.a1 << " A2 " << band.a2;
        } else if (band.type == EQBandType::GeneralNotch) {
            ss << "Fc " << static_cast<int>(band.freqNotch) << " Hz Fp " << static_cast<int>(band.freqPole) << " Hz Qp " << std::setprecision(2) << band.qPole << " Norm " << (band.normalizeAtDc ? 1 : 0);
        } else if (band.type == EQBandType::LinkwitzTransform) {
            ss << "Fa " << std::setprecision(1) << band.freqAct << " Hz Qa " << std::setprecision(3) << band.qAct << " Ft " << std::setprecision(1) << band.freqTarget << " Hz Qt " << std::setprecision(3) << band.qTarget;
        } else {
            ss << "Fc " << static_cast<int>(band.freq) << " Hz ";
            if (eqBandTypeHasGain(band.type)) {
                ss << "Gain " << std::setprecision(1) << band.gain << " dB ";
            }
            if (eqBandTypeHasQ(band.type)) {
                if (band.useSlope) {
                    ss << "S " << std::setprecision(1) << band.slope;
                } else if (band.useBandwidth) {
                    ss << "BW " << std::setprecision(2) << band.bandwidth;
                } else {
                    ss << "Q " << std::setprecision(2) << band.q;
                }
            }
        }
        ss << "\n";
    }
    return ss.str();
}

std::optional<EQPreset> EQPreset::fromCSV(const std::string& text, const std::string& presetName) {
    std::vector<EQBand> parsedBands;
    double preamp = 0.0;

    std::stringstream ss(text);
    std::string line;

    while (std::getline(ss, line)) {
        // Trim whitespace
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(first, (last - first + 1));

        if (trimmed.empty() || trimmed[0] == '#') continue;

        std::string lower = trimmed;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.find("preamp") == 0) {
            size_t pos = trimmed.find_first_of(":-= ");
            if (pos != std::string::npos) {
                std::string valStr = trimmed.substr(pos + 1);
                size_t dbPos = valStr.find("dB");
                if (dbPos == std::string::npos) dbPos = valStr.find("db");
                if (dbPos == std::string::npos) dbPos = valStr.find("DB");
                if (dbPos != std::string::npos) valStr.erase(dbPos);
                try { preamp = std::stod(valStr); } catch (...) {}
            }
            continue;
        }

        if (lower.find("filter") == 0) {
            size_t pos = trimmed.find(':');
            std::string content = (pos != std::string::npos) ? trimmed.substr(pos + 1) : trimmed.substr(6);
            std::stringstream lineSS(content);
            std::vector<std::string> words;
            std::string word;
            while (lineSS >> word) words.push_back(word);

            if (!words.empty()) {
                size_t idx = 0;
                try {
                    (void)std::stoi(words[0]);
                    if (words.size() > 1) idx = 1;
                } catch (...) {}

                if (idx < words.size()) {
                    bool enabled = true;
                    std::string stateWord = words[idx];
                    std::transform(stateWord.begin(), stateWord.end(), stateWord.begin(), ::toupper);

                    if (stateWord == "ON" || stateWord == "OFF") {
                        enabled = (stateWord == "ON");
                        idx++;
                    }

                    if (idx < words.size()) {
                        EQBandType type = shortNameToEQBandType(words[idx]);
                        EQBand band(type);
                        band.isEnabled = enabled;

                        for (size_t i = idx + 1; i + 1 < words.size(); ++i) {
                            std::string k = words[i];
                            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                            std::string v = words[i + 1];

                            try {
                                if (k == "fc") { band.freq = std::stod(v); band.freqNotch = band.freq; }
                                else if (k == "gain") { band.gain = std::stod(v); }
                                else if (k == "q") { band.q = std::stod(v); }
                                else if (k == "s" || k == "slope") { band.slope = std::stod(v); band.useSlope = true; }
                                else if (k == "bw" || k == "bandwidth") { band.bandwidth = std::stod(v); band.useBandwidth = true; }
                                else if (k == "fp") { band.freqPole = std::stod(v); }
                                else if (k == "qp") { band.qPole = std::stod(v); }
                                else if (k == "norm") { band.normalizeAtDc = (std::stod(v) != 0.0); }
                                else if (k == "fa") { band.freqAct = std::stod(v); }
                                else if (k == "qa") { band.qAct = std::stod(v); }
                                else if (k == "ft") { band.freqTarget = std::stod(v); }
                                else if (k == "qt") { band.qTarget = std::stod(v); }
                                else if (k == "b0") { band.b0 = std::stod(v); }
                                else if (k == "b1") { band.b1 = std::stod(v); }
                                else if (k == "b2") { band.b2 = std::stod(v); }
                                else if (k == "a1") { band.a1 = std::stod(v); }
                                else if (k == "a2") { band.a2 = std::stod(v); }
                            } catch (...) {}
                        }
                        parsedBands.push_back(band);
                    }
                }
            }
        }
    }

    if (parsedBands.empty()) return std::nullopt;
    return EQPreset(presetName, preamp, parsedBands);
}

QJsonObject EQPreset::toJson() const {
    QJsonObject obj;
    obj["id"] = id.toString();
    obj["name"] = QString::fromStdString(name);
    obj["preampGain"] = preampGain;
    QJsonArray arr;
    for (const auto& b : bands) arr.append(b.toJson());
    obj["bands"] = arr;
    return obj;
}

EQPreset EQPreset::fromJson(const QJsonObject& json) {
    EQPreset p;
    if (json.contains("id")) p.id = QUuid::fromString(json["id"].toString());
    if (json.contains("name")) p.name = json["name"].toString().toStdString();
    if (json.contains("preampGain")) p.preampGain = json["preampGain"].toDouble();
    if (json.contains("bands")) {
        QJsonArray arr = json["bands"].toArray();
        for (const auto& val : arr) {
            p.bands.push_back(EQBand::fromJson(val.toObject()));
        }
    }
    return p;
}

bool EQPreset::operator==(const EQPreset& other) const {
    return id == other.id && name == other.name && preampGain == other.preampGain && bands == other.bands;
}
