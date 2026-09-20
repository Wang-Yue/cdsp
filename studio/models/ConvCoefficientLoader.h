#ifndef CONV_COEFFICIENT_LOADER_H
#define CONV_COEFFICIENT_LOADER_H

#include <optional> // for optional
#include <stddef.h> // for size_t
#include <string>   // for string, basic_string
#include <vector>   // for vector

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

    static std::vector<double> loadWAV(const std::string& path, int channel = 0);
    static std::vector<double> loadRaw(const std::string& path, const std::string& format = "FLOAT64",
                                       int skipBytesLines = 0, int readBytesLines = 0);

    static void normalizeGain(std::vector<double>& coeffs, double targetDb = 0.0);

    static std::vector<double> loadCoefficients(const std::string& path, const std::string& format = "AUTO",
                                                int channel = 0, int skipBytesLines = 0, int readBytesLines = 0);

    static bool saveRawFloat64(const std::vector<double>& coeffs, const std::string& outputPath);
};

#endif // CONV_COEFFICIENT_LOADER_H
