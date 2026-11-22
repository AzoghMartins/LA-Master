#include "Modules.h"

namespace lam
{
namespace
{
/** Applies a per-channel filter pair according to a stereo / mid / side mode. */
template <typename F0, typename F1>
inline void applyMode (int mode, float& a, float& b, F0&& f0, F1&& f1) noexcept
{
    if (mode == (int) ChannelMode::stereo)
    {
        a = f0 (a);
        b = f1 (b);
        return;
    }
    float m = 0.5f * (a + b), s = 0.5f * (a - b);
    if (mode == (int) ChannelMode::mid)
        m = f0 (m);
    else
        s = f0 (s);
    a = m + s;
    b = m - s;
}
} // namespace

//==============================================================================
// EQ
void EqModule::design (Biquad& b, double sr, int band, float freq, float gainDb, float q) noexcept
{
    if (band == 0)
        b.setLowShelf (sr, freq, gainDb, clampf (q, 0.3f, 1.5f));
    else if (band == kEqBands - 1)
        b.setHighShelf (sr, freq, gainDb, clampf (q, 0.3f, 1.5f));
    else
        b.setPeak (sr, freq, std::max (0.1f, q), gainDb);
}

void EqModule::prepare (double sampleRate)
{
    sr = sampleRate;
    first = true;
    setParams (params);
    reset();
}

void EqModule::reset()
{
    for (auto& band : filters)
        for (auto& f : band) f.reset();
    for (auto& f : hpf) f.reset();
    for (auto& f : lpf) f.reset();
}

void EqModule::setParams (const EqParams& p) noexcept
{
    for (int b = 0; b < kEqBands; ++b)
    {
        const auto k = (size_t) b;
        if (first || p.freq[k] != params.freq[k] || p.gain[k] != params.gain[k] || p.q[k] != params.q[k])
            for (auto& f : filters[k])
                design (f, sr, b, p.freq[k], p.gain[k], p.q[k]);
        active[k] = std::abs (p.gain[k]) > 0.01f;
    }
    const bool hOn = p.hpfHz > 10.5f, lOn = p.lpfHz < 21500.0f && p.lpfHz < sr * 0.45;
    if (first || p.hpfHz != params.hpfHz)
        for (auto& f : hpf) f.setHighPass (sr, std::max (10.0f, p.hpfHz));
    if (first || p.lpfHz != params.lpfHz)
        for (auto& f : lpf) f.setLowPass (sr, std::min ((double) p.lpfHz, sr * 0.45));
    if (hOn && ! hpfOn) for (auto& f : hpf) f.reset();
    if (lOn && ! lpfOn) for (auto& f : lpf) f.reset();
    hpfOn = hOn;
    lpfOn = lOn;
    params = p;
    first = false;
}

void EqModule::process (float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        float a = l[i], b = r[i];
        if (hpfOn)
        {
            a = hpf[0].process (a);
            b = hpf[1].process (b);
        }
        for (int k = 0; k < kEqBands; ++k)
        {
            if (! active[(size_t) k])
                continue;
            auto& f = filters[(size_t) k];
            applyMode (params.mode[(size_t) k], a, b, [&f] (float x) { return f[0].process (x); },
                       [&f] (float x) { return f[1].process (x); });
        }
        if (lpfOn)
        {
            a = lpf[0].process (a);
            b = lpf[1].process (b);
        }
        l[i] = a;
        r[i] = b;
    }
}

//==============================================================================
// Dynamic EQ
void DynamicEqModule::prepare (double sampleRate)
{
    sr = sampleRate;
    needDesign = true;
    setParams (params);
    reset();
}

void DynamicEqModule::reset()
{
    for (auto* set : { &detect, &filters })
        for (auto& band : *set)
            for (auto& f : band) f.reset();
    env.fill (0.0f);
    gainDb.fill (0.0f);
    appliedDb.fill (0.0f);
    for (auto& band : filters)
        for (auto& f : band) f.setBypass();
    counter = 0;
}

void DynamicEqModule::setParams (const DynEqParams& p) noexcept
{
    for (int k = 0; k < kDynEqBands; ++k)
    {
        const auto b = (size_t) k;
        if (needDesign || p.freq[b] != params.freq[b] || p.q[b] != params.q[b])
        {
            for (auto& f : detect[b]) f.setBandPass (sr, p.freq[b], std::max (0.3f, p.q[b]));
            appliedDb[b] = 1000.0f; // force a redesign of the audio filter
        }
    }
    att = msCoeff (sr, std::max (0.1f, p.attackMs));
    rel = msCoeff (sr, std::max (5.0f, p.releaseMs));
    params = p;
    needDesign = false;
}

void DynamicEqModule::process (float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        float a = l[i], b = r[i];
        for (int k = 0; k < kDynEqBands; ++k)
        {
            const auto bk = (size_t) k;
            if (std::abs (params.range[bk]) < 0.05f)
                continue;
            float d;
            auto& det = detect[bk];
            switch (params.mode[bk])
            {
                case (int) ChannelMode::mid:  d = std::abs (det[0].process (0.5f * (a + b))); break;
                case (int) ChannelMode::side: d = std::abs (det[0].process (0.5f * (a - b))); break;
                default: d = std::max (std::abs (det[0].process (a)), std::abs (det[1].process (b))); break;
            }
            env[bk] = d + (d > env[bk] ? att : rel) * (env[bk] - d);
        }

        if (--counter <= 0)
        {
            counter = 16;
            for (int k = 0; k < kDynEqBands; ++k)
            {
                const auto bk = (size_t) k;
                float g = 0.0f;
                if (std::abs (params.range[bk]) >= 0.05f)
                {
                    const float over = gainToDb (env[bk]) - params.threshold[bk];
                    g = over > 0.0f ? params.range[bk] * (1.0f - std::exp (-over / 6.0f)) : 0.0f;
                }
                gainDb[bk] = g;
                if (std::abs (g - appliedDb[bk]) > 0.02f)
                {
                    for (auto& f : filters[bk])
                        f.setPeak (sr, params.freq[bk], std::max (0.3f, params.q[bk]), g);
                    appliedDb[bk] = g;
                }
            }
        }

        for (int k = 0; k < kDynEqBands; ++k)
        {
            const auto bk = (size_t) k;
            if (std::abs (appliedDb[bk]) < 0.01f)
                continue;
            auto& f = filters[bk];
            applyMode (params.mode[bk], a, b, [&f] (float x) { return f[0].process (x); },
                       [&f] (float x) { return f[1].process (x); });
        }
        l[i] = a;
        r[i] = b;
    }
}

//==============================================================================
// Stabilizer
void StabilizerModule::prepare (double sampleRate)
{
    sr = sampleRate;
    for (int b = 0; b < kNumBands; ++b)
        for (auto& f : analysis[(size_t) b])
            f.setBandPass (sr, std::min ((double) kBandFreqs[(size_t) b], sr * 0.45), 1.41);
    setParams (params);
    reset();
}

void StabilizerModule::reset()
{
    for (auto* set : { &analysis, &eq })
        for (auto& band : *set)
            for (auto& f : band) f.reset();
    for (auto& band : eq)
        for (auto& f : band) f.setBypass();
    power.fill (0.0);
    gains.fill (0.0f);
    applied.fill (0.0f);
    counter = 0;
}

void StabilizerModule::setParams (const StabilizerParams& p) noexcept
{
    params = p;
    coeff = msCoeff (sr, std::max (50.0f, p.speedMs));
}

void StabilizerModule::updateFilters() noexcept
{
    BandCurve level {};
    for (int b = 0; b < kNumBands; ++b)
        level[(size_t) b] = (float) (10.0 * std::log10 (power[(size_t) b] + 1.0e-14));
    const auto norm = normaliseCurve (level);
    const float mean = level[2] - norm[2];

    BandCurve target {};
    for (int b = 0; b < kNumBands; ++b)
        target[(size_t) b] = params.target[(size_t) b] + params.tilt * std::log2 (kBandFreqs[(size_t) b] / 1000.0f);
    target = normaliseCurve (target);

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto k = (size_t) b;
        float g = 0.0f;
        if (mean > -80.0f)
        {
            g = clampf (-params.amount * (norm[k] - target[k]), -maxCut, maxBoost);
            if (norm[k] < -40.0f) g = std::min (g, 0.0f); // never boost an (almost) empty band
        }
        gains[k] = g;
        if (std::abs (g - applied[k]) > 0.05f)
        {
            for (auto& f : eq[k])
                f.setPeak (sr, std::min ((double) kBandFreqs[k], sr * 0.45), 1.0, g);
            applied[k] = g;
        }
    }
}

void StabilizerModule::process (float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float m = 0.5f * (l[i] + r[i]);
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& f = analysis[(size_t) b];
            const float y = f[1].process (f[0].process (m));
            auto& pw = power[(size_t) b];
            pw = (double) y * y + coeff * (pw - (double) y * y);
        }
        if (--counter <= 0)
        {
            counter = 64;
            updateFilters();
        }
        float a = l[i], c = r[i];
        for (int b = 0; b < kNumBands; ++b)
        {
            if (std::abs (applied[(size_t) b]) < 0.01f)
                continue;
            a = eq[(size_t) b][0].process (a);
            c = eq[(size_t) b][1].process (c);
        }
        l[i] = a;
        r[i] = c;
    }
}

//==============================================================================
// Glue compressor
void GlueModule::prepare (double sampleRate)
{
    sr = sampleRate;
    makeup.setTime (sr, 20.0f);
    mix.setTime (sr, 20.0f);
    slowUp = msCoeff (sr, 400.0f);
    slowDown = msCoeff (sr, 1500.0f);
    setParams (params);
    reset();
}

void GlueModule::reset()
{
    for (auto& f : scHp) f.reset();
    grFast = grSlow = grDb = 0.0f;
    makeup.snap (dbToGain (params.makeupDb));
    mix.snap (params.mix);
}

void GlueModule::setParams (const GlueParams& p) noexcept
{
    if (p.scHpfHz != params.scHpfHz || p.scHpfHz > 20.5f)
        for (auto& f : scHp) f.setHighPass (sr, std::max (20.0f, p.scHpfHz));
    att = msCoeff (sr, std::max (0.05f, p.attackMs));
    rel = msCoeff (sr, p.releaseMs > 0.0f ? p.releaseMs : 120.0f);
    params = p;
}

void GlueModule::process (float* l, float* r, int n) noexcept
{
    const bool hp = params.scHpfHz > 20.5f;
    const bool autoRel = params.releaseMs <= 0.0f;
    const float mk = dbToGain (params.makeupDb);
    for (int i = 0; i < n; ++i)
    {
        const float sl = hp ? scHp[0].process (l[i]) : l[i];
        const float sr2 = hp ? scHp[1].process (r[i]) : r[i];
        const float lvl = gainToDb (std::max (std::abs (sl), std::abs (sr2)));
        const float t = MultibandModule::staticReduction (lvl, params.thresholdDb, params.ratio, 6.0f);
        grFast = t + (t > grFast ? att : rel) * (grFast - t);
        float gr = grFast;
        if (autoRel)
        {
            grSlow = grFast + (grFast > grSlow ? slowUp : slowDown) * (grSlow - grFast);
            gr = std::max (grFast, grSlow);
        }
        grDb = gr;
        const float g = dbToGain (-gr) * makeup.next (mk);
        const float m = mix.next (params.mix);
        l[i] += m * (l[i] * g - l[i]);
        r[i] += m * (r[i] * g - r[i]);
    }
}

//==============================================================================
// Multiband dynamics
float MultibandModule::staticReduction (float level, float thr, float ratio, float knee) noexcept
{
    if (ratio <= 1.0f)
        return 0.0f;
    const float slope = 1.0f - 1.0f / ratio;
    const float over = level - thr;
    if (2.0f * over < -knee)
        return 0.0f;
    if (2.0f * std::abs (over) <= knee)
    {
        const float x = over + knee * 0.5f;
        return slope * x * x / (2.0f * knee);
    }
    return slope * over;
}

void MultibandModule::prepare (double sampleRate)
{
    sr = sampleRate;
    lastLow = lastHigh = -1.0f;
    for (auto& g : bandGain) g.setTime (sr, 20.0f);
    mix.setTime (sr, 20.0f);
    setParams (params);
    reset();
}

void MultibandModule::reset()
{
    for (auto& s : split) s.reset();
    gr.fill (0.0f);
    grOut.fill (0.0f);
    for (int k = 0; k < kMbBands; ++k)
        bandGain[(size_t) k].snap (dbToGain (params.gainDb[(size_t) k]));
    mix.snap (params.mix);
}

void MultibandModule::setParams (const MultibandParams& p) noexcept
{
    if (p.lowMidHz != lastLow || p.midHighHz != lastHigh)
    {
        for (auto& s : split) s.setFrequencies (sr, p.lowMidHz, p.midHighHz);
        lastLow = p.lowMidHz;
        lastHigh = p.midHighHz;
    }
    for (int k = 0; k < kMbBands; ++k)
    {
        att[(size_t) k] = msCoeff (sr, std::max (0.1f, p.attackMs[(size_t) k]));
        rel[(size_t) k] = msCoeff (sr, std::max (5.0f, p.releaseMs[(size_t) k]));
    }
    params = p;
}

void MultibandModule::process (float* l, float* r, int n) noexcept
{
    const bool anySolo = params.solo[0] || params.solo[1] || params.solo[2];
    std::array<float, kMbBands> target {};
    for (int k = 0; k < kMbBands; ++k) target[(size_t) k] = dbToGain (params.gainDb[(size_t) k]);

    for (int i = 0; i < n; ++i)
    {
        float lb[kMbBands], rb[kMbBands];
        split[0].process (l[i], lb[0], lb[1], lb[2]);
        split[1].process (r[i], rb[0], rb[1], rb[2]);
        float wl = 0.0f, wr = 0.0f, dl = 0.0f, dr = 0.0f;
        for (int k = 0; k < kMbBands; ++k)
        {
            const auto b = (size_t) k;
            const float lvl = gainToDb (std::max (std::abs (lb[k]), std::abs (rb[k])));
            const float t = staticReduction (lvl, params.threshold[b], params.ratio[b], 6.0f);
            gr[b] = t + (t > gr[b] ? att[b] : rel[b]) * (gr[b] - t);
            const float g = dbToGain (-gr[b]) * bandGain[b].next (target[b]);
            dl += lb[k];
            dr += rb[k];
            if (! anySolo || params.solo[b])
            {
                wl += lb[k] * g;
                wr += rb[k] * g;
            }
        }
        if (anySolo)
        {
            l[i] = wl;
            r[i] = wr;
        }
        else
        {
            const float m = mix.next (params.mix);
            l[i] = dl + m * (wl - dl);
            r[i] = dr + m * (wr - dr);
        }
    }
    grOut = gr;
}

//==============================================================================
// Exciter
float ExciterModule::shape (ExciterMode mode, float x) noexcept
{
    switch (mode)
    {
        case ExciterMode::tape:   return std::tanh (x);
        case ExciterMode::tube:   return std::tanh (x + 0.2f * x * x);
        case ExciterMode::warm:   return x / (1.0f + std::abs (x));
        case ExciterMode::triode: return x >= 0.0f ? 1.0f - std::exp (-x) : (std::exp (1.4f * x) - 1.0f) / 1.4f;
        case ExciterMode::retro:  return std::abs (x) < 1.0f ? x - x * x * x / 3.0f : (x > 0.0f ? 2.0f / 3.0f : -2.0f / 3.0f);
        case ExciterMode::numModes: break;
    }
    return x;
}

void ExciterModule::prepare (double sampleRate)
{
    sr = sampleRate;
    lastLow = lastHigh = -1.0f;
    for (auto& d : bypassDelay)
    {
        d.prepare (getLatency() + 4);
        d.setDelay (getLatency());
    }
    for (auto& f : dcBlock) f.setHighPass (sr, 8.0, 0.70710678);
    for (auto* set : { &drive, &mix })
        for (auto& s : *set) s.setTime (sr, 30.0f);
    setParams (params);
    reset();
}

void ExciterModule::reset()
{
    for (auto& o : os) o.reset();
    for (auto& s : split) s.reset();
    for (auto& d : bypassDelay) d.reset();
    for (auto& f : dcBlock) f.reset();
    for (int k = 0; k < kMbBands; ++k)
    {
        drive[(size_t) k].snap (dbToGain (params.driveDb[(size_t) k]));
        mix[(size_t) k].snap (params.mix[(size_t) k]);
    }
}

void ExciterModule::setParams (const ExciterParams& p) noexcept
{
    if (p.lowMidHz != lastLow || p.midHighHz != lastHigh)
    {
        for (auto& s : split) s.setFrequencies (sr * 2.0, p.lowMidHz, p.midHighHz);
        lastLow = p.lowMidHz;
        lastHigh = p.midHighHz;
    }
    params = p;
}

void ExciterModule::process (float* l, float* r, int n) noexcept
{
    float* io[2] = { l, r };
    std::array<float, kMbBands> dTarget {};
    for (int k = 0; k < kMbBands; ++k) dTarget[(size_t) k] = dbToGain (params.driveDb[(size_t) k]);

    for (int i = 0; i < n; ++i)
    {
        float g[kMbBands], m[kMbBands];
        for (int k = 0; k < kMbBands; ++k)
        {
            g[k] = drive[(size_t) k].next (dTarget[(size_t) k]);
            m[k] = mix[(size_t) k].next (params.mix[(size_t) k]);
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            const float x = io[ch][i];
            const float delayed = bypassDelay[(size_t) ch].process (x);
            float u[2];
            os[(size_t) ch].upsample (x, u[0], u[1]);
            if (! params.enabled)
            {
                os[(size_t) ch].downsample (u[0], u[1]); // keep the filters warm
                io[ch][i] = delayed;
                continue;
            }
            for (auto& v : u)
            {
                float b[kMbBands];
                split[(size_t) ch].process (v, b[0], b[1], b[2]);
                float sum = 0.0f;
                for (int k = 0; k < kMbBands; ++k)
                    sum += b[k] + m[k] * (shape (params.mode, b[k] * g[k]) / g[k] - b[k]);
                v = sum;
            }
            io[ch][i] = dcBlock[(size_t) ch].process (os[(size_t) ch].downsample (u[0], u[1]));
        }
    }
}

//==============================================================================
// Imager
void ImagerModule::prepare (double sampleRate)
{
    sr = sampleRate;
    lastLow = lastHigh = lastMono = -1.0f;
    for (auto& w : width) w.setTime (sr, 30.0f);
    stereoize.setTime (sr, 30.0f);
    stDelay.prepare ((int) (sr * 0.02) + 8);
    stHp.setHighPass (sr, 300.0, 0.70710678);
    setParams (params);
    reset();
}

void ImagerModule::reset()
{
    for (auto& s : split) s.reset();
    for (auto* b : { &monoLp1, &monoLp2, &monoApM, &monoApS, &stHp }) b->reset();
    stDelay.reset();
    for (int k = 0; k < kMbBands; ++k) width[(size_t) k].snap (params.width[(size_t) k]);
    stereoize.snap (params.stereoize);
}

void ImagerModule::setParams (const ImagerParams& p) noexcept
{
    if (p.lowMidHz != lastLow || p.midHighHz != lastHigh)
    {
        for (auto& s : split) s.setFrequencies (sr, p.lowMidHz, p.midHighHz);
        lastLow = p.lowMidHz;
        lastHigh = p.midHighHz;
    }
    if (p.monoBassHz != lastMono)
    {
        const double f = std::max (20.0f, p.monoBassHz);
        monoLp1.setLowPass (sr, f, 0.70710678);
        monoLp2.setLowPass (sr, f, 0.70710678);
        monoApM.setAllPass (sr, f, 0.70710678);
        monoApS.setAllPass (sr, f, 0.70710678);
        lastMono = p.monoBassHz;
    }
    params = p;
}

void ImagerModule::process (float* l, float* r, int n) noexcept
{
    const bool mono = params.monoBassHz > 20.5f;
    const int stTap = (int) (sr * 0.009);
    for (int i = 0; i < n; ++i)
    {
        float lb[kMbBands], rb[kMbBands];
        split[0].process (l[i], lb[0], lb[1], lb[2]);
        split[1].process (r[i], rb[0], rb[1], rb[2]);
        float m = 0.0f, s = 0.0f;
        for (int k = 0; k < kMbBands; ++k)
        {
            m += 0.5f * (lb[k] + rb[k]);
            s += 0.5f * (lb[k] - rb[k]) * width[(size_t) k].next (clampf (params.width[(size_t) k], 0.0f, 2.0f));
        }
        if (mono)
        {
            // HP(s) = AP(s) - LP(s) for Linkwitz-Riley filters: mid gets the same allpass so the image stays coherent
            const float low = monoLp2.process (monoLp1.process (s));
            s = monoApS.process (s) - low;
            m = monoApM.process (m);
        }
        const float st = stereoize.next (params.stereoize);
        stDelay.push (m);
        const float d = stHp.process (stDelay.readInt (stTap));
        s += st * 0.7f * d;
        l[i] = m + s;
        r[i] = m - s;
    }
}

//==============================================================================
// Maximizer
void SlidingMin::prepare (int windowLength)
{
    cap = std::max (2, windowLength + 1);
    size = std::max (1, windowLength);
    vals.assign ((size_t) cap, 1.0f);
    idx.assign ((size_t) cap, 0);
    reset();
}

void SlidingMin::reset() noexcept
{
    head = tail = 0;
    t = 0;
}

float SlidingMin::push (float v) noexcept
{
    ++t;
    while (head != tail)
    {
        const int back = (tail - 1 + cap) % cap;
        if (vals[(size_t) back] < v)
            break;
        tail = back;
    }
    vals[(size_t) tail] = v;
    idx[(size_t) tail] = t;
    tail = (tail + 1) % cap;
    while (idx[(size_t) head] <= t - size)
        head = (head + 1) % cap;
    return vals[(size_t) head];
}

void MaximizerModule::prepare (double sampleRate)
{
    sr = sampleRate;
    lookahead = std::max (8, (int) std::lround (sr * 0.002));
    for (auto& d : clipBypass)
    {
        d.prepare (Oversampler2x::latency() + 4);
        d.setDelay (Oversampler2x::latency());
    }
    for (auto& d : peakDelay) d.prepare (TruePeakDetector::delay() + 4);
    for (auto& d : audioDelay) d.prepare (lookahead + TruePeakDetector::delay() + 8);
    for (auto& w : windowMin) w.prepare (lookahead + 2);
    for (auto& b : box) b.assign ((size_t) lookahead, 1.0);
    gain.setTime (sr, 30.0f);
    setParams (params);
    reset();
}

void MaximizerModule::reset()
{
    for (auto& o : os) o.reset();
    for (auto& d : clipBypass) d.reset();
    for (auto& t : tp) t.reset();
    for (auto& d : peakDelay) d.reset();
    for (auto& d : audioDelay) d.reset();
    for (auto& w : windowMin) w.reset();
    for (auto& b : box) std::fill (b.begin(), b.end(), 1.0);
    boxSum.fill ((double) lookahead);
    boxPos = 0;
    env.fill (1.0f);
    gain.snap (params.enabled ? dbToGain (params.gainDb) : 1.0f);
    grDb = 0.0f;
}

void MaximizerModule::setParams (const MaximizerParams& p) noexcept
{
    params = p;
}

float MaximizerModule::clipSample (float x, float threshold, float kneeStart) const noexcept
{
    const float ax = std::abs (x);
    if (ax <= kneeStart)
        return x;
    const float span = threshold - kneeStart;
    const float y = kneeStart + span * std::tanh ((ax - kneeStart) / span);
    return x < 0.0f ? -y : y;
}

void MaximizerModule::process (float* l, float* r, int n) noexcept
{
    float* io[2] = { l, r };
    const bool on = params.enabled;
    const float ceiling = dbToGain (params.ceilingDb);
    const float target = on ? dbToGain (params.gainDb) : 1.0f;
    const bool clipping = on && (params.mode == MaxMode::loud || params.mode == MaxMode::clip);
    const float clipT = params.mode == MaxMode::clip ? ceiling : ceiling * 1.26f;
    const float clipK = clipT * (params.mode == MaxMode::clip ? 0.85f : 0.75f);
    static const float releaseMult[] = { 1.6f, 1.0f, 0.6f, 0.45f };
    const float relC = msCoeff (sr, std::max (1.0f, params.releaseMs) * releaseMult[(int) params.mode % 4]);
    const float link = clampf (params.link, 0.0f, 1.0f);
    const int D = TruePeakDetector::delay();
    float minGain = 1.0f;

    for (int i = 0; i < n; ++i)
    {
        const float g = gain.next (target);
        float pk[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto c = (size_t) ch;
            const float x = io[ch][i] * g;
            float u0, u1;
            os[c].upsample (x, u0, u1);
            const float bypassed = clipBypass[c].process (x);
            float y;
            if (clipping)
                y = os[c].downsample (clipSample (u0, clipT, clipK), clipSample (u1, clipT, clipK));
            else
            {
                os[c].downsample (u0, u1);
                y = bypassed;
            }
            const float t = tp[c].process (y);
            peakDelay[c].push (std::abs (y));
            const float sp = peakDelay[c].readInt (D + 1);
            pk[ch] = params.truePeak ? std::max (t, sp) : sp;
            audioDelay[c].push (y);
        }

        const float p0 = std::max (pk[0], link * pk[1]);
        const float p1 = std::max (pk[1], link * pk[0]);
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto c = (size_t) ch;
            const float p = ch == 0 ? p0 : p1;
            const float req = on && p > ceiling ? ceiling / p : 1.0f;
            const float mn = windowMin[c].push (req);
            env[c] = mn < env[c] ? mn : mn + relC * (env[c] - mn);
            boxSum[c] += (double) env[c] - box[c][(size_t) boxPos];
            box[c][(size_t) boxPos] = env[c];
            const float gg = std::min (1.0f, (float) (boxSum[c] / (double) lookahead));
            float y = audioDelay[c].readInt (lookahead + D + 1) * gg;
            if (on)
                y = clampf (y, -ceiling, ceiling);
            io[ch][i] = y;
            minGain = std::min (minGain, gg);
        }
        boxPos = (boxPos + 1) % lookahead;
    }
    grDb = -gainToDb (minGain);
}

//==============================================================================
// Dither
void Dither::process (float* l, float* r, int n, DitherMode mode) noexcept
{
    if (mode == DitherMode::off)
        return;
    const float q = mode == DitherMode::bits24 ? 1.0f / 8388608.0f : 1.0f / 32768.0f;
    const bool shaped = mode == DitherMode::bits16Shaped;
    static const float c[3] = { 1.623f, -0.982f, 0.109f };
    float* io[2] = { l, r };
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& e = err[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            float x = io[ch][i];
            if (shaped)
                x -= c[0] * e[0] + c[1] * e[1] + c[2] * e[2];
            const float d = (rng.nextFloat() - rng.nextFloat()) * q;
            float y = std::round ((x + d) / q) * q;
            y = clampf (y, -1.0f, 1.0f - q);
            if (shaped)
            {
                e[2] = e[1];
                e[1] = e[0];
                e[0] = clampf (y - x, -4.0f * q, 4.0f * q);
            }
            io[ch][i] = y;
        }
    }
}
} // namespace lam
