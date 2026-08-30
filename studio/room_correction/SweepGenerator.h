#ifndef SWEEP_GENERATOR_H
#define SWEEP_GENERATOR_H

#include <stddef.h> // for size_t
#include <utility>  // for pair
#include <vector>   // for vector

class SweepGenerator {
public:
    static std::vector<double> generate(double f1, double f2, double durationSeconds, int sampleRate,
                                        double fadeInSeconds = 0.05, double fadeOutSeconds = 0.05);

    static std::vector<double> inverseFilter(double f1, double f2, double durationSeconds, int sampleRate);

    static std::pair<std::vector<double>, std::vector<double>> sweepAndInverse(double f1, double f2,
                                                                               double durationSeconds, int sampleRate,
                                                                               double fadeInSeconds = 0.05,
                                                                               double fadeOutSeconds = 0.05);

private:
    static void applyTapers(std::vector<double>& buffer, size_t fadeInSamples, size_t fadeOutSamples);
};

#endif // SWEEP_GENERATOR_H
