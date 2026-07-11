#include "models/ConvolutionPreset.h"
#include <cmath>
#include <algorithm>
#include <QJsonArray>

ConvolutionPreset::ConvolutionPreset() : id(QUuid::createUuid()), name("Untitled IR") {}

ConvolutionPreset::ConvolutionPreset(const std::string& name, const std::map<int, std::string>& irPaths, int taps, const std::string& kindLabel)
    : id(QUuid::createUuid()), name(name), irPaths(irPaths), taps(taps), kindLabelStr(kindLabel) {}

std::string ConvolutionPreset::irPath(int sampleRate) const {
    auto it = irPaths.find(sampleRate);
    if (it != irPaths.end()) return it->second;
    if (!irPaths.empty()) return irPaths.begin()->second;
    return "";
}

std::vector<int> ConvolutionPreset::availableSampleRates() const {
    std::vector<int> rates;
    for (const auto& [rate, path] : irPaths) {
        rates.push_back(rate);
    }
    return rates;
}

double ConvolutionPreset::latencyMilliseconds(int sampleRate) const {
    if (sampleRate <= 0) return 0.0;
    return (static_cast<double>(taps) / 2.0 / static_cast<double>(sampleRate)) * 1000.0;
}

QJsonObject ConvolutionPreset::toJson() const {
    QJsonObject obj;
    obj["id"] = id.toString();
    obj["name"] = QString::fromStdString(name);
    obj["taps"] = taps;
    obj["kindLabel"] = QString::fromStdString(kindLabelStr);

    QJsonObject pathsObj;
    for (const auto& [rate, path] : irPaths) {
        pathsObj[QString::number(rate)] = QString::fromStdString(path);
    }
    obj["irPaths"] = pathsObj;

    return obj;
}

ConvolutionPreset ConvolutionPreset::fromJson(const QJsonObject& json) {
    ConvolutionPreset preset;
    if (json.contains("id")) preset.id = QUuid::fromString(json["id"].toString());
    if (json.contains("name")) preset.name = json["name"].toString().toStdString();
    if (json.contains("taps")) preset.taps = json["taps"].toInt();
    if (json.contains("kindLabel")) preset.kindLabelStr = json["kindLabel"].toString().toStdString();

    if (json.contains("irPaths")) {
        QJsonObject pathsObj = json["irPaths"].toObject();
        for (auto it = pathsObj.begin(); it != pathsObj.end(); ++it) {
            int rate = it.key().toInt();
            preset.irPaths[rate] = it.value().toString().toStdString();
        }
    }
    return preset;
}

bool ConvolutionPreset::operator==(const ConvolutionPreset& other) const {
    return id == other.id && name == other.name && taps == other.taps && irPaths == other.irPaths;
}
