#ifndef CONV_COEFFICIENT_LOADER_H
#define CONV_COEFFICIENT_LOADER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct WavHeaderInfo {
    int sampleRate = 48000;
    int channels = 1;
    int bitsPerSample = 32;
    bool isFloat = true;
    size_t dataOffset = 44;
    size_t dataSize = 0;
};

class ConvCoefficientLoader {
public:
    static std::optional<WavHeaderInfo> parseWavHeader(const std::string& path);

    static std::vector<double> loadCoefficients(const std::string& path, const std::string& format = "AUTO",
                                                int channel = 0, int userSampleRate = 48000);

    static bool saveRawFloat64(const std::vector<double>& coeffs, const std::string& outputPath);
};

#endif // CONV_COEFFICIENT_LOADER_H
