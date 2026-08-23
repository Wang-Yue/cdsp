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
            file.seekg(6, std::ios::cur);
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

        file.seekg(chunkDataPos + static_cast<std::streamoff>(chunkSize + (chunkSize % 2)));
    }

    if (foundFmt && foundData)
        return info;
    return std::nullopt;
}

std::vector<double> ConvCoefficientLoader::loadWAV(const std::string& path, int targetChannel) {
    std::vector<double> coeffs;
    auto wavInfo = parseWavHeader(path);
    if (!wavInfo.has_value())
        return coeffs;

    const auto& w = wavInfo.value();
    if (targetChannel < 0 || targetChannel >= w.channels)
        return coeffs;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return coeffs;

    file.seekg(w.dataOffset);
    if (w.bitsPerSample < 8)
        return coeffs;

    int chs = std::max(1, w.channels);
    int selCh = std::min(chs - 1, std::max(0, targetChannel));
    size_t totalSamples = w.dataSize / (w.bitsPerSample / 8);
    size_t frameCount = totalSamples / chs;

    coeffs.reserve(frameCount);

    if (w.isFloat && w.bitsPerSample == 32) {
        std::vector<float> frameBuffer(chs);
        for (size_t i = 0; i < frameCount; ++i) {
            if (!file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * sizeof(float)))
                break;
            coeffs.push_back(static_cast<double>(frameBuffer[selCh]));
        }
    } else if (w.isFloat && w.bitsPerSample == 64) {
        std::vector<double> frameBuffer(chs);
        for (size_t i = 0; i < frameCount; ++i) {
            if (!file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * sizeof(double)))
                break;
            coeffs.push_back(frameBuffer[selCh]);
        }
    } else if (!w.isFloat && w.bitsPerSample == 16) {
        std::vector<int16_t> frameBuffer(chs);
        for (size_t i = 0; i < frameCount; ++i) {
            if (!file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * sizeof(int16_t)))
                break;
            coeffs.push_back(static_cast<double>(frameBuffer[selCh]) / 32767.0);
        }
    } else if (!w.isFloat && w.bitsPerSample == 24) {
        std::vector<uint8_t> frameBuffer(chs * 3);
        for (size_t i = 0; i < frameCount; ++i) {
            if (!file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * 3))
                break;
            size_t idx = selCh * 3;
            int32_t val = (frameBuffer[idx + 2] << 16) | (frameBuffer[idx + 1] << 8) | frameBuffer[idx];
            if (val & 0x800000)
                val |= 0xFF000000;
            coeffs.push_back(static_cast<double>(val) / 8388607.0);
        }
    } else if (!w.isFloat && w.bitsPerSample == 32) {
        std::vector<int32_t> frameBuffer(chs);
        for (size_t i = 0; i < frameCount; ++i) {
            if (!file.read(reinterpret_cast<char*>(frameBuffer.data()), chs * sizeof(int32_t)))
                break;
            coeffs.push_back(static_cast<double>(frameBuffer[selCh]) / 2147483647.0);
        }
    }
    return coeffs;
}

std::vector<double> ConvCoefficientLoader::loadRaw(const std::string& path, const std::string& fmt, int skipBytesLines,
                                                   int readBytesLines) {
    std::vector<double> coeffs;
    if (fmt == "TEXT") {
        std::ifstream textFile(path);
        if (!textFile.is_open())
            return coeffs;
        std::string line;
        int currentLine = 0;
        int readCount = 0;
        while (std::getline(textFile, line)) {
            if (skipBytesLines > 0 && currentLine < skipBytesLines) {
                currentLine++;
                continue;
            }
            if (readBytesLines > 0 && readCount >= readBytesLines)
                break;

            size_t p1 = line.find_first_not_of(" \t\r\n");
            if (p1 != std::string::npos) {
                try {
                    coeffs.push_back(std::stod(line.substr(p1)));
                    readCount++;
                } catch (...) {
                }
            }
            currentLine++;
        }
        return coeffs;
    }

    std::ifstream rawFile(path, std::ios::binary);
    if (!rawFile.is_open())
        return coeffs;

    if (skipBytesLines > 0) {
        rawFile.seekg(skipBytesLines, std::ios::beg);
    }

    size_t startPos = rawFile.tellg();
    rawFile.seekg(0, std::ios::end);
    size_t totalBytes = static_cast<size_t>(rawFile.tellg()) - startPos;
    rawFile.seekg(startPos, std::ios::beg);

    size_t bytesToRead = totalBytes;
    if (readBytesLines > 0 && static_cast<size_t>(readBytesLines) < bytesToRead) {
        bytesToRead = static_cast<size_t>(readBytesLines);
    }

    if (fmt == "FLOAT64" || fmt == "F64_LE") {
        size_t count = bytesToRead / sizeof(double);
        coeffs.resize(count);
        rawFile.read(reinterpret_cast<char*>(coeffs.data()), count * sizeof(double));
    } else if (fmt == "FLOAT32" || fmt == "F32_LE") {
        size_t count = bytesToRead / sizeof(float);
        std::vector<float> fBuf(count);
        rawFile.read(reinterpret_cast<char*>(fBuf.data()), count * sizeof(float));
        coeffs.reserve(count);
        for (float val : fBuf)
            coeffs.push_back(static_cast<double>(val));
    } else if (fmt == "S32_LE") {
        size_t count = bytesToRead / sizeof(int32_t);
        std::vector<int32_t> iBuf(count);
        rawFile.read(reinterpret_cast<char*>(iBuf.data()), count * sizeof(int32_t));
        coeffs.reserve(count);
        for (int32_t val : iBuf)
            coeffs.push_back(static_cast<double>(val) / 2147483647.0);
    } else if (fmt == "S16_LE") {
        size_t count = bytesToRead / sizeof(int16_t);
        std::vector<int16_t> iBuf(count);
        rawFile.read(reinterpret_cast<char*>(iBuf.data()), count * sizeof(int16_t));
        coeffs.reserve(count);
        for (int16_t val : iBuf)
            coeffs.push_back(static_cast<double>(val) / 32767.0);
    }

    return coeffs;
}

void ConvCoefficientLoader::normalizeGain(std::vector<double>& coeffs, double targetDb) {
    if (coeffs.empty())
        return;
    double maxVal = 0.0;
    for (double c : coeffs) {
        double absVal = std::abs(c);
        if (absVal > maxVal)
            maxVal = absVal;
    }
    if (maxVal > 0.0) {
        double scale = std::pow(10.0, targetDb / 20.0) / maxVal;
        for (double& c : coeffs) {
            c *= scale;
        }
    }
}

std::vector<double> ConvCoefficientLoader::loadCoefficients(const std::string& path, const std::string& fmt,
                                                            int targetChannel, int skipBytesLines, int readBytesLines) {
    std::string upperFmt = fmt;
    std::transform(upperFmt.begin(), upperFmt.end(), upperFmt.begin(), ::toupper);

    if (upperFmt == "WAV" || parseWavHeader(path).has_value()) {
        return loadWAV(path, targetChannel);
    }
    return loadRaw(path, upperFmt == "AUTO" ? "FLOAT64" : upperFmt, skipBytesLines, readBytesLines);
}

bool ConvCoefficientLoader::saveRawFloat64(const std::vector<double>& coeffs, const std::string& outputPath) {
    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open())
        return false;
    out.write(reinterpret_cast<const char*>(coeffs.data()), coeffs.size() * sizeof(double));
    return out.good();
}
