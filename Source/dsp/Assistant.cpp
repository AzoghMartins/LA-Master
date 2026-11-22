#include "Assistant.h"

#include <cstdio>
#include <memory>

namespace lam
{
const char* targetStyleName (TargetStyle s) noexcept
{
    switch (s)
    {
        case TargetStyle::balanced:   return "Balanced";
        case TargetStyle::pop:        return "Pop";
        case TargetStyle::edm:        return "EDM";
        case TargetStyle::hiphop:     return "Hip-Hop";
        case TargetStyle::rock:       return "Rock";
        case TargetStyle::acoustic:   return "Acoustic";
        case TargetStyle::orchestral: return "Orchestral";
        case TargetStyle::lofi:       return "Lo-Fi";
        case TargetStyle::reference:  return "Reference";
        case TargetStyle::numStyles:  break;
    }
    return "";
}

BandCurve targetCurve (TargetStyle s) noexcept
{
    // Average long-term octave band balance of commercial masters, relative to pink noise (dB).
    //                                 31.5    63    125   250   500    1k    2k    4k    8k    16k
    static const float curves[][kNumBands] = {
        /* Balanced   */ {  4.0f,  5.0f,  4.0f,  2.0f,  0.0f, -1.0f, -3.0f, -5.5f, -8.0f, -12.0f },
        /* Pop        */ {  3.5f,  5.0f,  3.5f,  1.5f,  0.0f, -1.0f, -2.5f, -4.5f, -7.0f, -10.5f },
        /* EDM        */ {  7.0f,  8.0f,  5.0f,  2.0f,  0.0f, -1.5f, -3.0f, -5.0f, -7.5f, -11.0f },
        /* Hip-Hop    */ {  8.0f,  8.0f,  5.0f,  2.0f,  0.0f, -1.5f, -3.5f, -6.0f, -8.5f, -12.0f },
        /* Rock       */ {  2.0f,  4.0f,  4.0f,  2.5f,  0.5f,  0.0f, -2.0f, -4.5f, -7.5f, -12.0f },
        /* Acoustic   */ {  0.0f,  2.0f,  3.0f,  2.0f,  0.0f, -1.0f, -3.0f, -5.5f, -8.5f, -13.0f },
        /* Orchestral */ { -1.0f,  1.5f,  2.5f,  2.0f,  0.0f, -1.5f, -4.0f, -7.0f, -10.0f, -15.0f },
        /* Lo-Fi      */ {  3.0f,  5.0f,  4.5f,  3.0f,  0.5f, -1.0f, -4.0f, -8.0f, -13.0f, -20.0f },
    };
    int i = (int) s;
    if (i < 0 || i >= (int) TargetStyle::reference)
        i = 0;
    BandCurve c {};
    for (int b = 0; b < kNumBands; ++b) c[(size_t) b] = curves[i][b];
    return normaliseCurve (c);
}

TrackAnalysis analyseTrack (const float* l, const float* r, int n, double sr)
{
    TrackAnalysis a;
    if (n <= 0 || sr <= 0.0)
        return a;
    LoudnessMeter meter;
    meter.prepare (sr);
    BandAnalyzer bands;
    bands.prepare (sr);
    TruePeakDetector tpl, tpr;
    float tp = 0.0f;
    for (int pos = 0; pos < n; pos += 4096)
    {
        const int k = std::min (4096, n - pos);
        meter.process (l + pos, r + pos, k);
        bands.process (l + pos, r + pos, k);
        for (int i = pos; i < pos + k; ++i)
            tp = std::max ({ tp, tpl.process (l[i]), tpr.process (r[i]) });
    }
    a.integratedLufs = meter.getIntegrated();
    a.range = meter.getRange();
    a.truePeakDb = gainToDb (tp);
    a.curve = bands.getLevels (true);
    a.seconds = (double) n / sr;
    a.valid = a.integratedLufs > -70.0f;
    return a;
}

namespace
{
struct LoudnessResult { float lufs = kSilenceLufs, truePeakDb = -100.0f; };

/** Integrated loudness of `pre` after a maximizer with the given gain (latency removed). */
LoudnessResult measureMaximized (const std::vector<float>& pl, const std::vector<float>& pr, double sr, MaximizerParams mp, float gainDb)
{
    MaximizerModule m;
    mp.gainDb = gainDb;
    mp.enabled = true;
    m.setParams (mp);
    m.prepare (sr);
    m.reset();
    std::vector<float> l (pl), r (pr);
    const int n = (int) l.size();
    for (int pos = 0; pos < n; pos += 2048)
        m.process (l.data() + pos, r.data() + pos, std::min (2048, n - pos));
    const int lat = m.getLatency();
    LoudnessResult res;
    if (n <= lat)
        return res;
    LoudnessMeter meter;
    meter.prepare (sr);
    TruePeakDetector tl, tr;
    float tp = 0.0f;
    meter.process (l.data() + lat, r.data() + lat, n - lat);
    for (int i = lat; i < n; ++i)
        tp = std::max ({ tp, tl.process (l[(size_t) i]), tr.process (r[(size_t) i]) });
    res.lufs = meter.getIntegrated();
    res.truePeakDb = gainToDb (tp);
    return res;
}

/** 95th percentile of 50 ms RMS (dBFS) of each of the three bands. */
std::array<float, kMbBands> bandLevels (const float* l, const float* r, int n, double sr, float lowMid, float midHigh, float gain)
{
    std::array<ThreeBandSplitter, 2> split;
    for (auto& s : split) s.setFrequencies (sr, lowMid, midHigh);
    const int frame = std::max (64, (int) (sr * 0.05));
    std::array<std::vector<float>, kMbBands> values;
    std::array<double, kMbBands> acc {};
    int count = 0;
    for (int i = 0; i < n; ++i)
    {
        float lb[3], rb[3];
        split[0].process (l[i] * gain, lb[0], lb[1], lb[2]);
        split[1].process (r[i] * gain, rb[0], rb[1], rb[2]);
        for (int k = 0; k < kMbBands; ++k) acc[(size_t) k] += 0.5 * ((double) lb[k] * lb[k] + (double) rb[k] * rb[k]);
        if (++count == frame)
        {
            for (int k = 0; k < kMbBands; ++k)
            {
                values[(size_t) k].push_back ((float) (10.0 * std::log10 (acc[(size_t) k] / frame + 1.0e-14)));
                acc[(size_t) k] = 0.0;
            }
            count = 0;
        }
    }
    std::array<float, kMbBands> out {};
    for (int k = 0; k < kMbBands; ++k)
    {
        auto& v = values[(size_t) k];
        if (v.empty()) { out[(size_t) k] = -100.0f; continue; }
        std::sort (v.begin(), v.end());
        out[(size_t) k] = v[(size_t) ((v.size() - 1) * 95 / 100)];
    }
    return out;
}

/** Side energy relative to mid energy below `hz` (dB). */
float lowSideRatioDb (const float* l, const float* r, int n, double sr, float hz)
{
    Biquad m1, m2, s1, s2;
    for (auto* b : { &m1, &m2, &s1, &s2 }) b->setLowPass (sr, hz, 0.70710678);
    double em = 0.0, es = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float m = m2.process (m1.process (0.5f * (l[i] + r[i])));
        const float s = s2.process (s1.process (0.5f * (l[i] - r[i])));
        em += (double) m * m;
        es += (double) s * s;
    }
    return (float) (10.0 * std::log10 ((es + 1.0e-12) / (em + 1.0e-12)));
}

std::string fmt (const char* f, double a, double b = 0.0)
{
    char buf[96];
    std::snprintf (buf, sizeof (buf), f, a, b);
    return buf;
}
} // namespace

AssistResult runAssistant (const float* l, const float* r, int n, double sr, const EngineParams& current,
                           const BandCurve& target, float targetLufs)
{
    AssistResult res;
    res.params = current;
    const auto in = analyseTrack (l, r, n, sr);
    res.inputLufs = in.integratedLufs;
    if (! in.valid || in.integratedLufs < -60.0f || in.seconds < 2.0)
    {
        res.summary = "not enough audio";
        return res;
    }

    auto& p = res.params;
    p.bypass = p.delta = false;
    p.monitor = MonitorMode::stereo;
    p.dither = current.dither;

    // ---- Gain staging: bring the mix to about -18 LUFS before the chain --------------------------------
    p.inputGainDb = std::round (clampf (-18.0f - in.integratedLufs, -12.0f, 12.0f) * 10.0f) / 10.0f;
    const float inGain = dbToGain (p.inputGainDb);
    const float stagedLufs = in.integratedLufs + p.inputGainDb;

    // ---- Tonal balance: broad EQ moves towards the target, the Stabilizer handles the rest -------------
    BandCurve dev {};
    for (int b = 0; b < kNumBands; ++b) dev[(size_t) b] = in.curve[(size_t) b] - target[(size_t) b];
    const float bandDev[kEqBands] = { 0.5f * (dev[0] + dev[1]), dev[2], 0.5f * (dev[3] + dev[4]),
                                      0.5f * (dev[5] + dev[6]), dev[7], 0.5f * (dev[8] + dev[9]) };
    const float freqs[kEqBands] = { 70.0f, 160.0f, 450.0f, 1500.0f, 4500.0f, 11000.0f };
    const float qs[kEqBands] = { 0.7f, 0.8f, 0.7f, 0.7f, 0.8f, 0.7f };
    p.eq.enabled = true;
    float biggest = 0.0f;
    for (int b = 0; b < kEqBands; ++b)
    {
        float g = clampf (-0.6f * bandDev[b], -4.0f, 3.5f);
        if (b == kEqBands - 1 && in.curve[9] < -45.0f) g = std::min (g, 0.0f); // nothing up there to lift
        if (b == 0 && in.curve[0] < -45.0f) g = std::min (g, 0.0f);
        g = std::round (g * 10.0f) / 10.0f;
        p.eq.freq[(size_t) b] = freqs[b];
        p.eq.q[(size_t) b] = qs[b];
        p.eq.gain[(size_t) b] = g;
        p.eq.mode[(size_t) b] = (int) ChannelMode::stereo;
        if (std::abs (g) > std::abs (biggest)) biggest = g;
    }
    p.stab.enabled = true;
    p.stab.amount = 0.35f;
    p.stab.speedMs = 2000.0f;
    p.stab.tilt = 0.0f;
    p.stab.target = target;

    // ---- Dynamics --------------------------------------------------------------------------------------
    p.glue.enabled = true;
    p.glue.thresholdDb = clampf (stagedLufs + 3.0f, -40.0f, 0.0f);
    p.glue.ratio = 2.0f;
    p.glue.attackMs = 10.0f;
    p.glue.releaseMs = -1.0f;
    p.glue.makeupDb = 0.0f;
    p.glue.scHpfHz = 90.0f;
    p.glue.mix = 1.0f;

    const auto levels = bandLevels (l, r, n, sr, p.mb.lowMidHz, p.mb.midHighHz, inGain);
    p.mb.enabled = true;
    const float ratios[kMbBands] = { 2.0f, 1.7f, 1.6f };
    for (int k = 0; k < kMbBands; ++k)
    {
        const auto b = (size_t) k;
        p.mb.threshold[b] = clampf (std::round (levels[b] - 4.0f), -50.0f, 0.0f);
        p.mb.ratio[b] = ratios[k];
        p.mb.gainDb[b] = 0.0f;
        p.mb.solo[b] = false;
    }
    p.mb.attackMs = { 30.0f, 15.0f, 5.0f };
    p.mb.releaseMs = { 200.0f, 140.0f, 90.0f };
    p.mb.mix = 1.0f;

    // ---- Stereo: mono the sub-bass when it is wide ------------------------------------------------------
    const float lowSide = lowSideRatioDb (l, r, n, sr, 120.0f);
    const bool monoBass = lowSide > -15.0f;
    if (monoBass)
    {
        p.img.enabled = true;
        p.img.monoBassHz = 100.0f;
    }

    // ---- Maximizer: render the chain and search the gain that meets the loudness target -----------------
    p.max.enabled = true;
    p.max.ceilingDb = -1.0f;
    p.max.truePeak = true;
    p.max.mode = targetLufs > -10.0f ? MaxMode::loud : MaxMode::balanced;
    p.max.releaseMs = 120.0f;
    p.max.link = 1.0f;

    std::vector<float> pl (l, l + n), pr (r, r + n);
    {
        auto engine = std::make_unique<MasterEngine>();
        EngineParams q = p;
        q.max.enabled = false;
        q.dither = DitherMode::off;
        q.outputGainDb = 0.0f;
        engine->setParams (q);
        engine->prepare (sr, 2048);
        for (int pos = 0; pos < n; pos += 2048)
        {
            float* io[2] = { pl.data() + pos, pr.data() + pos };
            engine->process (io, 2, std::min (2048, n - pos), nullptr);
        }
    }

    float lo = 0.0f, hi = 24.0f;
    auto at0 = measureMaximized (pl, pr, sr, p.max, 0.0f);
    std::string note;
    if (at0.lufs > targetLufs + 0.3f)
    {
        p.max.gainDb = 0.0f;
        note = " (mix is louder than the target: lower the input or output gain)";
        res.resultLufs = at0.lufs;
        res.resultTruePeak = at0.truePeakDb;
    }
    else
    {
        auto top = measureMaximized (pl, pr, sr, p.max, hi);
        if (top.lufs < targetLufs)
        {
            p.max.gainDb = hi;
            res.resultLufs = top.lufs;
            res.resultTruePeak = top.truePeakDb;
            note = " (target not reachable)";
        }
        else
        {
            for (int it = 0; it < 11; ++it)
            {
                const float mid = 0.5f * (lo + hi);
                const auto m = measureMaximized (pl, pr, sr, p.max, mid);
                if (m.lufs < targetLufs) lo = mid;
                else hi = mid;
            }
            p.max.gainDb = std::round (hi * 10.0f) / 10.0f;
            const auto fin = measureMaximized (pl, pr, sr, p.max, p.max.gainDb);
            res.resultLufs = fin.lufs;
            res.resultTruePeak = fin.truePeakDb;
        }
    }

    res.ok = true;
    res.summary = fmt ("EQ up to %+.1f dB", biggest) + fmt (", glue %.0f dB", p.glue.thresholdDb)
                  + (monoBass ? ", mono bass 100 Hz" : "") + fmt (", maximizer +%.1f dB", p.max.gainDb)
                  + fmt (" -> %.1f LUFS", res.resultLufs) + note;
    if (std::abs (p.inputGainDb) > 0.05f)
        res.summary = fmt ("input %+.1f dB, ", p.inputGainDb) + res.summary;
    return res;
}
} // namespace lam
