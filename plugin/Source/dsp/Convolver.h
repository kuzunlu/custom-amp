#pragma once

#include "Common.h"

#include <juce_dsp/juce_dsp.h>

#include <vector>

namespace amp
{

// Convolution with an impulse response, with no latency.
// The first `partition` taps are done directly, sample by sample. The rest of the IR is done with
// the FFT in blocks of `partition` samples ("uniform partitioned convolution"): the result for the
// next block only needs input that has already arrived, so it is ready in time.
class Convolver
{
public:
    void load (const std::vector<double>& ir, int partitionSize = 128);
    void reset();

    double tick (double x) noexcept
    {
        history[(size_t) historyPos] = x;
        history[(size_t) (historyPos + partition)] = x;  // a second copy, so the taps read one straight run

        // history[historyPos + partition - k] is the input k samples ago, for k = 0 .. partition - 1.
        double y = 0.0;
        const double* recent = history.data() + historyPos + partition;

        for (size_t k = 0; k < head.size(); ++k)
            y += head[k] * recent[-(std::ptrdiff_t) k];

        historyPos = historyPos + 1 == partition ? 0 : historyPos + 1;

        if (numParts > 0)
        {
            y += tailOut[(size_t) blockPos];
            block[(size_t) (partition + blockPos)] = (float) x;

            if (++blockPos == partition)
                finishBlock();
        }

        return y;
    }

private:
    void finishBlock() noexcept;

    int partition = 128, numParts = 0, blockPos = 0, historyPos = 0, newest = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<double> head, history;
    std::vector<std::vector<float>> parts, spectra;  // IR partitions and recent input blocks, as spectra
    std::vector<float> block, work, accum, tailOut;
};

// Change the sample rate of a short signal (an IR) by cutting or padding its spectrum
// (resample_fft in ampsim/filters.py).
std::vector<double> resampleFft (const std::vector<double>& x, double rateFrom, double rateTo);

} // namespace amp
