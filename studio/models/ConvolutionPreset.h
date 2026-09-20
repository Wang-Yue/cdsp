#ifndef CONVOLUTION_PRESET_H
#define CONVOLUTION_PRESET_H

#include <QJsonObject> // for QJsonObject
#include <QUuid>       // for QUuid
#include <map>         // for map
#include <string>      // for basic_string, string
#include <vector>      // for vector

class ConvolutionPreset {
public:
    QUuid id;
    std::string name;
    std::map<int, std::string> irPaths;
    int taps = 0;
    std::string kindLabelStr = "User IR";

    ConvolutionPreset();
    ConvolutionPreset(const std::string& name, const std::map<int, std::string>& irPaths, int taps,
                      const std::string& kindLabel = "User IR");

    std::string irPath(int sampleRate) const;
    std::vector<int> availableSampleRates() const;
    double latencyMilliseconds(int sampleRate) const;
    std::string kindLabel() const { return kindLabelStr; }

    QJsonObject toJson() const;
    static ConvolutionPreset fromJson(const QJsonObject& json);

    bool operator==(const ConvolutionPreset& other) const;
};

#endif // CONVOLUTION_PRESET_H
