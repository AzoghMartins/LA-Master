#include "Loudness.h"

namespace lam
{
void KWeighting::prepare (double sr) noexcept
{
    // Coefficients derived from the analog prototypes of BS.1770 (valid at any sample rate).
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (kPi * f0 / sr);
        const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.setCoefficients ((Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                               2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (kPi * f0 / sr);
        const double a0 = 1.0 + K / Q + K * K;
        hp.setCoefficients (1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
    }
    reset();
}

//==============================================================================
void LoudnessMeter::prepare (double sampleRate)
{
    kl.prepare (sampleRate);
    kr.prepare (sampleRate);
    hopSize = std::max (1, (int) std::lround (sampleRate * 0.1));
    blockEnergy.assign ((size_t) kBins, 0.0);
    blockCount.assign ((size_t) kBins, 0);
    stCount.assign ((size_t) kBins, 0);
    reset();
}

void LoudnessMeter::reset() noexcept
{
    kl.reset();
    kr.reset();
    hopCount = 0;
    acc = 0.0;
    sub.fill (0.0);
    subPos = 0;
    hops = 0;
    momentary = shortTerm = kSilenceLufs;
    resetIntegrated();
}

void LoudnessMeter::resetIntegrated() noexcept
{
    std::fill (blockEnergy.begin(), blockEnergy.end(), 0.0);
    std::fill (blockCount.begin(), blockCount.end(), 0);
    std::fill (stCount.begin(), stCount.end(), 0);
    stTotal = 0;
    integrated = kSilenceLufs;
    range = 0.0f;
}

bool LoudnessMeter::process (const float* l, const float* r, int n) noexcept
{
    bool hop = false;
    for (int i = 0; i < n; ++i)
    {
        const float a = kl.process (l[i]), b = kr.process (r[i]);
        acc += (double) a * a + (double) b * b;
        if (++hopCount >= hopSize)
        {
            finishHop();
            hop = true;
        }
    }
    return hop;
}

void LoudnessMeter::finishHop() noexcept
{
    sub[(size_t) subPos] = acc / (double) hopSize;
    subPos = (subPos + 1) % (int) sub.size();
    acc = 0.0;
    hopCount = 0;
    ++hops;

    auto meanOfLast = [this] (int count)
    {
        double s = 0.0;
        for (int k = 1; k <= count; ++k)
            s += sub[(size_t) ((subPos - k + (int) sub.size()) % (int) sub.size())];
        return s / count;
    };

    auto binOf = [] (float lufs) { return std::min (kBins - 1, (int) ((lufs - kBinFloor) * 10.0f)); };

    if (hops >= 4)
    {
        const double p = meanOfLast (4);
        momentary = powerToLufs (p);
        if (momentary > kBinFloor)
        {
            const int b = binOf (momentary);
            blockEnergy[(size_t) b] += p;
            ++blockCount[(size_t) b];
            updateIntegrated();
        }
    }
    if (hops >= 30)
    {
        shortTerm = powerToLufs (meanOfLast (30));
        if (shortTerm > kBinFloor)
        {
            ++stCount[(size_t) binOf (shortTerm)];
            ++stTotal;
        }
    }
    else if (hops >= 4)
        shortTerm = powerToLufs (meanOfLast ((int) hops)); // ramp-in until 3 s are available
}

void LoudnessMeter::updateIntegrated() noexcept
{
    // Relative gate: -10 LU below the mean of all blocks above the absolute gate
    double e = 0.0;
    int64_t c = 0;
    for (int b = 0; b < kBins; ++b) { e += blockEnergy[(size_t) b]; c += blockCount[(size_t) b]; }
    if (c == 0)
        return;
    const float gate = powerToLufs (e / (double) c) - 10.0f;
    const int g0 = std::max (0, (int) std::floor ((gate - kBinFloor) * 10.0f));
    e = 0.0;
    c = 0;
    for (int b = g0; b < kBins; ++b) { e += blockEnergy[(size_t) b]; c += blockCount[(size_t) b]; }
    integrated = c > 0 ? powerToLufs (e / (double) c) : kSilenceLufs;

    // Loudness range (EBU Tech 3342): 10th to 95th percentile of gated short-term values
    if (stTotal > 0)
    {
        double se = 0.0;
        for (int b = 0; b < kBins; ++b)
            if (stCount[(size_t) b] > 0)
                se += (double) stCount[(size_t) b] * std::pow (10.0, ((kBinFloor + 0.1f * ((float) b + 0.5f)) + 0.691) / 10.0);
        const float stGate = powerToLufs (se / (double) stTotal) - 20.0f;
        const int s0 = std::max (0, (int) std::floor ((stGate - kBinFloor) * 10.0f));
        int64_t n = 0;
        for (int b = s0; b < kBins; ++b) n += stCount[(size_t) b];
        if (n > 0)
        {
            auto percentile = [&] (double pct)
            {
                const double want = pct * (double) n;
                int64_t a = 0;
                for (int b = s0; b < kBins; ++b)
                {
                    a += stCount[(size_t) b];
                    if ((double) a >= want)
                        return kBinFloor + 0.1f * ((float) b + 0.5f);
                }
                return kBinFloor + 0.1f * (float) kBins;
            };
            range = std::max (0.0f, percentile (0.95) - percentile (0.10));
        }
    }
}

//==============================================================================
TruePeakDetector::TruePeakDetector()
{
    constexpr int len = kTaps * 4;
    const double centre = (len - 1) * 0.5;
    std::array<double, len> h {};
    for (int j = 0; j < len; ++j)
    {
        const double t = ((double) j - centre) / 4.0;
        const double fc = 0.92; // slightly below the original Nyquist
        const double sinc = std::abs (t) < 1.0e-9 ? fc : std::sin (kPi * fc * t) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos (2.0 * kPi * (j + 0.5) / len) + 0.08 * std::cos (4.0 * kPi * (j + 0.5) / len);
        h[(size_t) j] = sinc * w;
    }
    for (int p = 0; p < 4; ++p)
    {
        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k) sum += h[(size_t) (p + 4 * k)];
        for (int idx = 0; idx < kTaps; ++idx)
            coeffs[(size_t) p][(size_t) idx] = (float) (h[(size_t) (p + 4 * (kTaps - 1 - idx))] / sum);
    }
}

//==============================================================================
void BandAnalyzer::prepare (double sampleRate)
{
    sr = sampleRate;
    for (int b = 0; b < kNumBands; ++b)
        for (auto& f : filters[(size_t) b])
            f.setBandPass (sr, std::min ((double) kBandFreqs[(size_t) b], sr * 0.45), 1.41);
    reset();
}

void BandAnalyzer::reset() noexcept
{
    for (auto& band : filters)
        for (auto& f : band) f.reset();
    energy.fill (0.0);
    samples = 0;
}

void BandAnalyzer::process (const float* l, const float* r, int n) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& f = filters[(size_t) b];
        double e = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float y = f[1].process (f[0].process (0.5f * (l[i] + r[i])));
            e += (double) y * y;
        }
        energy[(size_t) b] += e;
    }
    samples += n;
}

BandCurve BandAnalyzer::getLevels (bool normalise) const noexcept
{
    BandCurve c {};
    for (int b = 0; b < kNumBands; ++b)
        c[(size_t) b] = (float) (10.0 * std::log10 (energy[(size_t) b] / (double) std::max<int64_t> (1, samples) + 1.0e-14));
    return normalise ? normaliseCurve (c) : c;
}

BandCurve normaliseCurve (const BandCurve& c) noexcept
{
    float mean = 0.0f;
    for (int b = 2; b <= 7; ++b) mean += c[(size_t) b];
    mean /= 6.0f;
    BandCurve o {};
    for (int b = 0; b < kNumBands; ++b) o[(size_t) b] = c[(size_t) b] - mean;
    return o;
}
} // namespace lam
