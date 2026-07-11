#include "models/ConvCoefficientLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

std::optional<WavHeaderInfo> ConvCoefficientLoader::parseWavHeader(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::nullopt;

    char riffHeader[12];
    if (!file.read(riffHeader, 12))
        return std::nullopt;
    if (std::memcmp(riffHeader, "RIFF", 4) != 0 || std::memcmp(riffHeader + 8, "WAVE", 4) != 0) {
        return std::nullopt;
    }

    WavHeaderInfo info;
    bool foundFmt = false, foundData = false;

    while (file) {
        char chunkHeader[8];
        if (!file.read(chunkHeader, 8))
            break;

        uint32_t chunkSize = *reinterpret_cast<uint32_t*>(chunkHeader + 4);
        std::streampos chunkDataPos = file.tellg();

        if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
            uint16_t audioFormat = 0;
            uint16_t numChannels = 0;
            uint32_t sampleRate = 0;
            uint16_t bitsPerSample = 0;

            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            file.read(reinterpret_cast<char*>(&numChannels), 2);
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            file.seekg(6, std::ios::cur); // Skip byteRate and blockAlign
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);

            info.sampleRate = static_cast<int>(sampleRate);
            info.channels = static_cast<int>(numChannels);
            info.bitsPerSample = static_cast<int>(bitsPerSample);
            info.isFloat = (audioFormat == 3);
            foundFmt = true;
        } else if (std::memcmp(chunkHeader, "data", 4) == 0) {
            info.dataOffset = static_cast<size_t>(chunkDataPos);
            info.dataSize = chunkSize;
            foundData = true;
            break;
        }

        file.seekg(chunkDataPos + static_cast<std::streamoff>(chunkSize));
    }

    if (foundFmt && foundData)
        return info;
    return std::nullopt;
}

std::vector<double> ConvCoefficientLoader::loadCoefficients(const std::string& path, const std::string& fmt,
                                                            int targetChannel, int userSampleRate) {
    std::vector<double> coeffs;

    auto wavInfo = parseWavHeader(path);
    if (wavInfo.has_value()) {
        const auto& w = wavInfo.value();
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return coeffs;

        file.seekg(w.dataOffset);

        int chs = std::max(1, w.channels);
        int selCh = std::min(chs - 1, std::max(0, targetChannel));
        size_t totalSamples = w.dataSize / (w.bitsPerSample / 8);
        size_t frameCount = totalSamples / chs;

        coeffs.reserve(frameCount);

        if (w.isFloat && w.bitsPerSample == 32) {
            std::vector<float> frameBuffer(chs);
            for (size_t i = 0; i < frameCount; ++i) {
                file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * sizeof(float));
                coeffs.push_back(frameBuffer[selCh]);
            }
        } else if (w.isFloat && w.bitsPerSample == 64) {
            std::vector<double> frameBuffer(chs);
            for (size_t i = 0; i < frameCount; ++i) {
                file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * sizeof(double));
                coeffs.push_back(frameBuffer[selCh]);
            }
        } else if (!w.isFloat && w.bitsPerSample == 16) {
            std::vector<int16_t> frameBuffer(chs);
            for (size_t i = 0; i < frameCount; ++i) {
                file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * sizeof(int16_t));
                coeffs.push_back(static_cast<double>(frameBuffer[selCh]) / 32768.0);
            }
        } else if (!w.isFloat && w.bitsPerSample == 24) {
            std::vector<uint8_t> frameBuffer(chs * 3);
            for (size_t i = 0; i < frameCount; ++i) {
                file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * 3);
                size_t idx = selCh * 3;
                int32_t val = (frameBuffer[idx + 2] << 16) | (frameBuffer[idx + 1] << 8) | frameBuffer[idx];
                if (val & 0x800000)
                    val |= 0xFF000000; // Sign extend
                coeffs.push_back(static_cast<double>(val) / 8388608.0);
            }
        } else if (!w.isFloat && w.bitsPerSample == 32) {
            std::vector<int32_t> frameBuffer(chs);
            for (size_t i = 0; i < frameCount; ++i) {
                file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * sizeof(int32_t));
                coeffs.push_back(static_cast<double>(frameBuffer[selCh]) / 2147483648.0);
            }
        }
        return coeffs;
    }

    // Try reading as raw binary float64/float32 or text lines
    std::ifstream rawFile(path, std::ios::binary);
    if (!rawFile.is_open())
        return coeffs;

    rawFile.seekg(0, std::ios::end);
    size_t fileSize = rawFile.tellg();
    rawFile.seekg(0, std::ios::beg);

    if (fmt == "FLOAT64" || (fmt == "AUTO" && fileSize % sizeof(double) == 0)) {
        size_t count = fileSize / sizeof(double);
        coeffs.resize(count);
        rawFile.read(reinterpret_cast<char*>(coeffs.data()), fileSize);
        return coeffs;
    } else if (fmt == "FLOAT32" || (fmt == "AUTO" && fileSize % sizeof(float) == 0)) {
        size_t count = fileSize / sizeof(float);
        std::vector<float> fBuf(count);
        rawFile.read(reinterpret_cast<char*>(fBuf.data()), fileSize);
        coeffs.reserve(count);
        for (float val : fBuf)
            coeffs.push_back(static_cast<double>(val));
        return coeffs;
    }

    // Fallback to text reading
    rawFile.close();
    std::ifstream textFile(path);
    std::string line;
    while (std::getline(textFile, line)) {
        std::stringstream ss(line);
        double val;
        if (ss >> val) {
            coeffs.push_back(val);
        }
    }

    return coeffs;
}

bool ConvCoefficientLoader::saveRawFloat64(const std::vector<double>& coeffs, const std::string& outputPath) {
    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open())
        return false;
    out.write(reinterpret_cast<const char*>(coeffs.data()), coeffs.size() * sizeof(double));
    return out.good();
}
