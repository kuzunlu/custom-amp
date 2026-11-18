#include "Convolver.h"

#include <complex>

namespace amp
{

void Convolver::load (const std::vector<double>& ir, int partitionSize)
{
    partition = partitionSize;
    const int fftSize = 2 * partition;
    fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (fftSize)));

    head.assign (ir.begin(), ir.begin() + std::min (ir.size(), (size_t) partition));
    history.assign ((size_t) (2 * partition), 0.0);

    // The rest of the IR in pieces of `partition` taps, each zero-padded to fftSize and transformed.
    numParts = ir.size() > (size_t) partition ? (int) ((ir.size() - 1) / (size_t) partition) : 0;
    parts.assign ((size_t) numParts, std::vector<float> ((size_t) (2 * fftSize), 0.0f));

    for (int m = 0; m < numParts; ++m)
    {
        auto& p = parts[(size_t) m];

        for (int k = 0; k < partition; ++k)
        {
            const size_t tap = (size_t) ((m + 1) * partition + k);

            if (tap < ir.size())
                p[(size_t) k] = (float) ir[tap];
        }

        fft->performRealOnlyForwardTransform (p.data(), true);
    }

    spectra.assign ((size_t) numParts, std::vector<float> ((size_t) (2 * fftSize), 0.0f));
    block.assign ((size_t) fftSize, 0.0f);
    work.assign ((size_t) (2 * fftSize), 0.0f);
    accum.assign ((size_t) (2 * fftSize), 0.0f);
    tailOut.assign ((size_t) partition, 0.0f);
    reset();
}

void Convolver::reset()
{
    std::fill (history.begin(), history.end(), 0.0);
    std::fill (block.begin(), block.end(), 0.0f);
    std::fill (tailOut.begin(), tailOut.end(), 0.0f);

    for (auto& s : spectra)
        std::fill (s.begin(), s.end(), 0.0f);

    blockPos = historyPos = newest = 0;
}

void Convolver::finishBlock() noexcept
{
    // block = [previous input block, this input block]: overlap-save, so the last half of the result is right.
    std::copy (block.begin(), block.end(), work.begin());
    std::fill (work.begin() + (std::ptrdiff_t) block.size(), work.end(), 0.0f);
    fft->performRealOnlyForwardTransform (work.data(), true);

    newest = newest + 1 == numParts ? 0 : newest + 1;
    std::copy (work.begin(), work.end(), spectra[(size_t) newest].begin());

    // Next block's output from the IR tail: input block j - i through IR partition i + 1.
    std::fill (accum.begin(), accum.end(), 0.0f);
    const int bins = partition + 1;

    for (int i = 0; i < numParts; ++i)
    {
        const auto& x = spectra[(size_t) ((newest - i + numParts) % numParts)];
        const auto& h = parts[(size_t) i];

        for (int k = 0; k < bins; ++k)
        {
            const float xr = x[(size_t) (2 * k)], xi = x[(size_t) (2 * k + 1)];
            const float hr = h[(size_t) (2 * k)], hi = h[(size_t) (2 * k + 1)];
            accum[(size_t) (2 * k)] += xr * hr - xi * hi;
            accum[(size_t) (2 * k + 1)] += xr * hi + xi * hr;
        }
    }

    fft->performRealOnlyInverseTransform (accum.data());
    std::copy (accum.begin() + partition, accum.begin() + 2 * partition, tailOut.begin());

    // This block becomes the "previous" half of the next one.
    std::copy (block.begin() + partition, block.end(), block.begin());
    blockPos = 0;
}

//==============================================================================
using Complex = std::complex<double>;

// In-place radix-2 FFT (size must be a power of 2). inverse = no 1/n scaling.
static void fftRadix2 (std::vector<Complex>& a, bool inverse)
{
    const size_t n = a.size();

    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;

        for (; j & bit; bit >>= 1)
            j ^= bit;

        j ^= bit;

        if (i < j)
            std::swap (a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = 2 * juce::MathConstants<double>::pi / (double) len * (inverse ? 1 : -1);

        for (size_t start = 0; start < n; start += len)
            for (size_t k = 0; k < len / 2; ++k)
            {
                const Complex w = std::polar (1.0, angle * (double) k);
                const Complex u = a[start + k], v = a[start + k + len / 2] * w;
                a[start + k] = u + v;
                a[start + k + len / 2] = u - v;
            }
    }
}

// DFT of any length (Bluestein's method): X[k] = sum over j of x[j] * exp(-2 pi i j k / n).
static std::vector<Complex> dft (const std::vector<Complex>& x)
{
    const size_t n = x.size();
    size_t m = 1;

    while (m < 2 * n - 1)
        m <<= 1;

    // chirp[j] = exp(-i pi j^2 / n), with j^2 taken modulo 2n so the angle stays exact.
    std::vector<Complex> chirp (n);

    for (size_t j = 0; j < n; ++j)
    {
        const auto jj = (unsigned long long) j * j % (2 * n);
        chirp[j] = std::polar (1.0, -juce::MathConstants<double>::pi * (double) jj / (double) n);
    }

    std::vector<Complex> a (m, 0.0), b (m, 0.0);

    for (size_t j = 0; j < n; ++j)
        a[j] = x[j] * chirp[j];

    b[0] = std::conj (chirp[0]);

    for (size_t j = 1; j < n; ++j)
        b[j] = b[m - j] = std::conj (chirp[j]);

    fftRadix2 (a, false);
    fftRadix2 (b, false);

    for (size_t i = 0; i < m; ++i)
        a[i] *= b[i];

    fftRadix2 (a, true);
    std::vector<Complex> out (n);

    for (size_t k = 0; k < n; ++k)
        out[k] = a[k] / (double) m * chirp[k];

    return out;
}

std::vector<double> resampleFft (const std::vector<double>& x, double rateFrom, double rateTo)
{
    // Python's round() rounds halves to even, like nearbyint.
    const auto nNew = (size_t) std::nearbyint ((double) x.size() * rateTo / rateFrom);
    const auto spectrum = dft (std::vector<Complex> (x.begin(), x.end()));
    const size_t keep = std::min (x.size() / 2 + 1, nNew / 2 + 1);

    // irfft: build the full two-sided spectrum. Bin 0 (and the top bin for an even length) is real.
    std::vector<Complex> full (nNew, 0.0);

    for (size_t k = 0; k < keep; ++k)
    {
        Complex v = spectrum[k];

        if (k == 0 || (nNew % 2 == 0 && k == nNew / 2))
            v = v.real();

        full[k] = v;

        if (k != 0 && nNew - k != k)
            full[nNew - k] = std::conj (v);
    }

    // inverse DFT = conj (DFT (conj (X))); irfft divides by nNew, then * nNew / len (x).
    for (auto& v : full)
        v = std::conj (v);

    const auto back = dft (full);
    std::vector<double> out (nNew);

    for (size_t i = 0; i < nNew; ++i)
        out[i] = back[i].real() / (double) x.size();

    return out;
}

} // namespace amp
