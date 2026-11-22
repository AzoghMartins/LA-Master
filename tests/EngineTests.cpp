// Headless tests for the LA-Master DSP engine (no JUCE needed).
#include "dsp/Assistant.h"
#include "dsp/MasterEngine.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace lam;

namespace
{
int failures = 0;
void check (bool ok, const std::string& what)
{
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (! ok)
        ++failures;
}

std::string num (double v, int decimals = 2)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.*f", decimals, v);
    return buf;
}

constexpr double sr = 48000.0;

struct Stereo
{
    std::vector<float> l, r;
    size_t size() const { return l.size(); }
};

Stereo sine (double hz, double seconds, float amp, double phaseR = 0.0)
{
    Stereo s;
    const size_t n = (size_t) (seconds * sr);
    s.l.resize (n);
    s.r.resize (n);
    for (size_t i = 0; i < n; ++i)
    {
        s.l[i] = amp * (float) std::sin (2.0 * kPi * hz * (double) i / sr);
        s.r[i] = amp * (float) std::sin (2.0 * kPi * hz * (double) i / sr + phaseR);
    }
    return s;
}

/** Pink-ish noise (Voss-McCartney style) with an optional tilt in dB/octave. */
Stereo noise (double seconds, float amp, unsigned seed = 1, float tiltDbPerOct = 0.0f)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    Stereo s;
    const size_t n = (size_t) (seconds * sr);
    s.l.resize (n);
    s.r.resize (n);
    // Paul Kellet's pink filter
    for (int ch = 0; ch < 2; ++ch)
    {
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        auto& v = ch == 0 ? s.l : s.r;
        for (size_t i = 0; i < n; ++i)
        {
            const float w = nd (rng);
            b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f; b2 = 0.96900f * b2 + w * 0.1538520f;
            b3 = 0.86650f * b3 + w * 0.3104856f; b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
            v[i] = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.11f;
            b6 = w * 0.115926f;
        }
    }
    if (std::abs (tiltDbPerOct) > 0.01f)
    {
        // a high shelf + low shelf pair roughly gives a tilt around 1 kHz
        Biquad hs[2], ls[2];
        for (auto& b : hs) b.setHighShelf (sr, 1000.0, tiltDbPerOct * 3.0, 0.5);
        for (auto& b : ls) b.setLowShelf (sr, 1000.0, -tiltDbPerOct * 3.0, 0.5);
        for (size_t i = 0; i < n; ++i)
        {
            s.l[i] = ls[0].process (hs[0].process (s.l[i]));
            s.r[i] = ls[1].process (hs[1].process (s.r[i]));
        }
    }
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) peak = std::max ({ peak, std::abs (s.l[i]), std::abs (s.r[i]) });
    for (size_t i = 0; i < n; ++i) { s.l[i] *= amp / peak; s.r[i] *= amp / peak; }
    return s;
}

/** A "mix": kick-like thumps, bass, chords and hats with a peaky crest factor. */
Stereo musicLike (double seconds, float amp = 0.5f, unsigned seed = 7)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    Stereo s;
    const size_t n = (size_t) (seconds * sr);
    s.l.assign (n, 0.0f);
    s.r.assign (n, 0.0f);
    const double bpm = 120.0, beat = 60.0 / bpm;
    Biquad hatHp[2];
    for (auto& b : hatHp) b.setHighPass (sr, 7000.0);
    const float chord[] = { 220.0f, 277.18f, 329.63f };
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const double tb = std::fmod (t, beat);
        const float kick = (float) (std::exp (-tb * 18.0) * std::sin (2.0 * kPi * (50.0 + 80.0 * std::exp (-tb * 30.0)) * tb));
        const float bass = 0.35f * (float) std::sin (2.0 * kPi * 55.0 * t) * (float) (0.6 + 0.4 * std::exp (-tb * 4.0));
        float pad = 0.0f;
        for (float f : chord) pad += 0.08f * (float) std::sin (2.0 * kPi * f * t);
        const double th = std::fmod (t + beat * 0.5, beat);
        const float hatEnv = (float) std::exp (-th * 60.0);
        const float hl = hatHp[0].process (nd (rng)) * 0.25f * hatEnv, hr = hatHp[1].process (nd (rng)) * 0.25f * hatEnv;
        const float padSide = 0.03f * (float) std::sin (2.0 * kPi * 0.3 * t);
        s.l[i] = amp * (0.9f * kick + bass + pad * (1.0f + padSide) + hl);
        s.r[i] = amp * (0.9f * kick + bass + pad * (1.0f - padSide) + hr);
    }
    return s;
}

EngineParams neutral()
{
    EngineParams p;
    p.eq.enabled = false;
    p.max.enabled = false;
    p.dither = DitherMode::off;
    p.gainMatch = false;
    return p;
}

void run (MasterEngine& e, Stereo& io, int block = 256, LoudnessListener* listener = nullptr)
{
    for (size_t s = 0; s < io.size(); s += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, io.size() - s);
        float* ptrs[2] = { io.l.data() + s, io.r.data() + s };
        e.process (ptrs, 2, n, listener);
    }
}

Stereo process (const EngineParams& p, Stereo in, int block = 256, double rate = sr)
{
    MasterEngine e;
    e.setParams (p);
    e.prepare (rate, 512);
    run (e, in, block);
    return in;
}

double rms (const std::vector<float>& x, size_t from, size_t to)
{
    double acc = 0.0;
    to = std::min (to, x.size());
    for (size_t i = from; i < to; ++i) acc += (double) x[i] * x[i];
    return std::sqrt (acc / (double) std::max<size_t> (1, to - from));
}

double dbRms (const std::vector<float>& x, size_t from, size_t to) { return 20.0 * std::log10 (rms (x, from, to) + 1.0e-12); }

/** Level of one frequency (single-bin DFT, dB of amplitude). */
double toneDb (const std::vector<float>& x, size_t from, size_t to, double hz)
{
    double re = 0.0, im = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * (double) (i - from) / (double) (to - from));
        re += w * x[i] * std::cos (2.0 * kPi * hz * (double) i / sr);
        im += w * x[i] * std::sin (2.0 * kPi * hz * (double) i / sr);
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (re * re + im * im) / ((double) (to - from) * 0.5) + 1.0e-12);
}

float integrated (const Stereo& s, size_t from = 0)
{
    LoudnessMeter m;
    m.prepare (sr);
    m.process (s.l.data() + from, s.r.data() + from, (int) (s.size() - from));
    return m.getIntegrated();
}

float samplePeak (const Stereo& s, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < s.size(); ++i) p = std::max ({ p, std::abs (s.l[i]), std::abs (s.r[i]) });
    return p;
}

float truePeakDb (const Stereo& s, size_t from = 0)
{
    TruePeakDetector a, b;
    float p = 0.0f;
    for (size_t i = from; i < s.size(); ++i) p = std::max ({ p, a.process (s.l[i]), b.process (s.r[i]) });
    return gainToDb (p);
}

struct FrameCounter : LoudnessListener
{
    int frames = 0;
    LoudnessFrame last;
    void loudnessFrame (const LoudnessFrame& f) override { ++frames; last = f; }
};
} // namespace

int main()
{
    std::printf ("LA-Master engine tests\n");

    // ---- Loudness metering (EBU R128 / BS.1770) ---------------------------------------------------
    {
        auto s = sine (997.0, 10.0, 0.1f); // -20 dBFS stereo sine
        LoudnessMeter m;
        m.prepare (sr);
        m.process (s.l.data(), s.r.data(), (int) s.size());
        check (std::abs (m.getIntegrated() + 20.0f) < 0.15f && std::abs (m.getShortTerm() + 20.0f) < 0.15f
                   && std::abs (m.getMomentary() + 20.0f) < 0.15f,
               "1 kHz sine at -20 dBFS reads -20 LUFS (I " + num (m.getIntegrated()) + ", S " + num (m.getShortTerm()) + ", M "
                   + num (m.getMomentary()) + ")");
        check (m.getRange() < 0.5f, "steady tone has no loudness range (" + num (m.getRange()) + " LU)");

        auto s44 = sine (997.0, 6.0, 0.1f);
        LoudnessMeter m44;
        m44.prepare (44100.0);
        std::vector<float> l44 ((size_t) (6 * 44100)), r44 (l44.size());
        for (size_t i = 0; i < l44.size(); ++i) l44[i] = r44[i] = 0.1f * (float) std::sin (2.0 * kPi * 997.0 * (double) i / 44100.0);
        m44.process (l44.data(), r44.data(), (int) l44.size());
        check (std::abs (m44.getIntegrated() + 20.0f) < 0.15f, "K-weighting is sample-rate independent (44.1 kHz: " + num (m44.getIntegrated()) + " LUFS)");

        // Gating: a passage 25 LU down does not pull the integrated value down
        auto loud = sine (997.0, 10.0, 0.1f);
        auto quiet = sine (997.0, 10.0, 0.1f * 0.0562f);
        auto medium = sine (997.0, 10.0, 0.1f * 0.316f);
        Stereo both { loud.l, loud.r };
        both.l.insert (both.l.end(), quiet.l.begin(), quiet.l.end());
        both.r.insert (both.r.end(), quiet.r.begin(), quiet.r.end());
        LoudnessMeter g;
        g.prepare (sr);
        g.process (both.l.data(), both.r.data(), (int) both.size());
        check (std::abs (g.getIntegrated() + 20.0f) < 0.3f, "relative gate ignores a passage 25 LU quieter (" + num (g.getIntegrated()) + " LUFS)");
        Stereo steps { loud.l, loud.r };
        steps.l.insert (steps.l.end(), medium.l.begin(), medium.l.end());
        steps.r.insert (steps.r.end(), medium.r.begin(), medium.r.end());
        LoudnessMeter lra;
        lra.prepare (sr);
        lra.process (steps.l.data(), steps.r.data(), (int) steps.size());
        check (std::abs (lra.getRange() - 10.0f) < 1.0f, "loudness range of a 10 LU step (" + num (lra.getRange()) + " LU)");
    }

    // ---- True peak --------------------------------------------------------------------------------
    {
        // fs/4 sine with 45 degree phase: samples hit 0.707, the waveform peaks at 1.0
        Stereo s;
        for (int i = 0; i < 4800; ++i)
        {
            const float v = (float) std::sin (2.0 * kPi * 0.25 * i + kPi / 4.0);
            s.l.push_back (v);
            s.r.push_back (v);
        }
        const float sp = gainToDb (samplePeak (s, 100)), tp = truePeakDb (s, 100);
        check (sp < -2.8f && std::abs (tp) < 0.5f, "true peak finds inter-sample peaks (sample " + num (sp) + " dB, true " + num (tp) + " dBTP)");
    }

    // ---- Crossover ---------------------------------------------------------------------------------
    {
        ThreeBandSplitter sp;
        sp.setFrequencies (sr, 200.0, 3000.0);
        std::vector<float> ir (16384);
        for (size_t i = 0; i < ir.size(); ++i)
        {
            float lo, mid, hi;
            sp.process (i == 0 ? 1.0f : 0.0f, lo, mid, hi);
            ir[i] = lo + mid + hi;
        }
        double worst = 0.0;
        for (double f : { 40.0, 120.0, 200.0, 500.0, 1500.0, 3000.0, 6000.0, 15000.0 })
        {
            double re = 0.0, im = 0.0;
            for (size_t i = 0; i < ir.size(); ++i)
            {
                re += ir[i] * std::cos (2.0 * kPi * f * (double) i / sr);
                im -= ir[i] * std::sin (2.0 * kPi * f * (double) i / sr);
            }
            worst = std::max (worst, std::abs (10.0 * std::log10 (re * re + im * im)));
        }
        check (worst < 0.05, "3-band Linkwitz-Riley split sums flat (max error " + num (worst, 3) + " dB)");
    }

    // ---- Neutral chain is a pure delay ----------------------------------------------------------------
    MasterEngine probe;
    probe.prepare (sr, 512);
    const int latency = probe.getLatency();
    {
        auto in = noise (1.0, 0.5f, 3);
        auto out = process (neutral(), in);
        double err = 0.0;
        for (size_t i = (size_t) latency; i < in.size(); ++i)
            err = std::max (err, (double) std::abs (out.l[i] - in.l[i - (size_t) latency]) + std::abs (out.r[i] - in.r[i - (size_t) latency]));
        check (latency > 0 && latency < sr * 0.004 && err < 1.0e-5,
               "neutral chain = pure delay of the reported latency (" + std::to_string (latency) + " samples, " + num (1000.0 * latency / sr) + " ms)");
    }
    {
        // latency is the same whatever is switched on (maximizer clean and with its oversampled clipper)
        bool same = true;
        std::string detail;
        for (auto mode : { MaxMode::transparent, MaxMode::loud })
        {
            EngineParams p = neutral();
            p.max.enabled = true;
            p.max.ceilingDb = 0.0f;
            p.max.mode = mode;
            Stereo imp;
            imp.l.assign (8192, 0.0f);
            imp.r.assign (8192, 0.0f);
            imp.l[1000] = imp.r[1000] = 0.5f;
            auto out = process (p, imp);
            size_t at = 0;
            for (size_t i = 0; i < out.size(); ++i)
                if (std::abs (out.l[i]) > std::abs (out.l[at])) at = i;
            same &= (int) at - 1000 == latency;
            detail += std::to_string ((int) at - 1000) + " ";
        }
        check (same, "maximizer on (clean / clipper): impulse arrives at the reported latency (" + detail + ")");
    }

    // ---- EQ ---------------------------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.eq.enabled = true;
        p.eq.freq[2] = 1000.0f;
        p.eq.gain[2] = 6.0f;
        p.eq.q[2] = 1.0f;
        auto out = process (p, sine (1000.0, 1.0, 0.1f));
        const double g = toneDb (out.l, 8000, 48000, 1000.0) - 20.0 * std::log10 (0.1);
        check (std::abs (g - 6.0) < 0.3, "EQ bell +6 dB at 1 kHz (" + num (g) + " dB)");

        p.eq.mode[2] = (int) ChannelMode::mid;
        auto side = sine (1000.0, 1.0, 0.1f, kPi); // L = -R: pure side
        auto outS = process (p, side);
        const double gs = toneDb (outS.l, 8000, 48000, 1000.0) - 20.0 * std::log10 (0.1);
        p.eq.mode[2] = (int) ChannelMode::side;
        auto outS2 = process (p, side);
        const double gs2 = toneDb (outS2.l, 8000, 48000, 1000.0) - 20.0 * std::log10 (0.1);
        check (std::abs (gs) < 0.2 && std::abs (gs2 - 6.0) < 0.3, "M/S EQ: mid band leaves the side alone (" + num (gs) + " dB), side band boosts it (" + num (gs2) + " dB)");

        EngineParams c = neutral();
        c.eq.enabled = true;
        c.eq.hpfHz = 40.0f;
        auto low = process (c, sine (20.0, 2.0, 0.3f));
        check (dbRms (low.l, 48000, 96000) - dbRms (sine (20.0, 2.0, 0.3f).l, 48000, 96000) < -20.0, "EQ high-pass at 40 Hz removes 20 Hz rumble");
    }

    // ---- Dynamic EQ ------------------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.dyneq.enabled = true;
        p.dyneq.freq[2] = 3000.0f;
        p.dyneq.q[2] = 1.5f;
        p.dyneq.threshold[2] = -30.0f;
        p.dyneq.range[2] = -6.0f;
        auto loud = process (p, sine (3000.0, 1.0, 0.5f));
        auto quiet = process (p, sine (3000.0, 1.0, 0.005f));
        const double gl = toneDb (loud.l, 24000, 48000, 3000.0) - 20.0 * std::log10 (0.5);
        const double gq = toneDb (quiet.l, 24000, 48000, 3000.0) - 20.0 * std::log10 (0.005);
        check (gl < -5.0 && gl > -7.0 && std::abs (gq) < 0.3, "dynamic EQ cuts a loud 3 kHz tone (" + num (gl) + " dB) but not a quiet one (" + num (gq) + " dB)");
    }

    // ---- Glue compressor -------------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.glue.enabled = true;
        p.glue.thresholdDb = -20.0f;
        p.glue.ratio = 4.0f;
        p.glue.scHpfHz = 20.0f;
        MasterEngine e;
        e.setParams (p);
        e.prepare (sr, 512);
        auto s = sine (1000.0, 1.0, 0.316f); // -10 dBFS peak
        run (e, s);
        const float gr = e.getMeters().glueDb;
        check (gr > 6.5f && gr < 8.0f, "glue: 10 dB over a 4:1 threshold gives ~7.5 dB reduction (" + num (gr) + " dB)");
        p.glue.mix = 0.0f;
        auto par = process (p, sine (1000.0, 1.0, 0.316f));
        check (std::abs (dbRms (par.l, 24000, 48000) - dbRms (sine (1000.0, 1.0, 0.316f).l, 24000, 48000)) < 0.1, "glue at 0 % mix is transparent");
    }

    // ---- Multiband dynamics -------------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.mb.enabled = true;
        p.mb.threshold = { 0.0f, 0.0f, -30.0f };
        p.mb.ratio = { 1.0f, 1.0f, 8.0f };
        Stereo mix = sine (100.0, 1.0, 0.3f);
        auto hi = sine (8000.0, 1.0, 0.3f);
        for (size_t i = 0; i < mix.size(); ++i) { mix.l[i] += hi.l[i]; mix.r[i] += hi.r[i]; }
        auto out = process (p, mix);
        const double low = toneDb (out.l, 24000, 48000, 100.0) - toneDb (mix.l, 24000, 48000, 100.0);
        const double high = toneDb (out.l, 24000, 48000, 8000.0) - toneDb (mix.l, 24000, 48000, 8000.0);
        check (std::abs (low) < 0.3 && high < -12.0, "multiband: high band compressed (" + num (high) + " dB), low band untouched (" + num (low) + " dB)");

        p.mb.solo = { true, false, false };
        auto solo = process (p, mix);
        check (toneDb (solo.l, 24000, 48000, 8000.0) < toneDb (mix.l, 24000, 48000, 8000.0) - 40.0, "multiband: soloing the low band removes the highs");
    }

    // ---- Exciter -------------------------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.exc.enabled = true;
        p.exc.driveDb = { 12.0f, 12.0f, 12.0f };
        p.exc.mix = { 1.0f, 1.0f, 1.0f };
        bool ok = true;
        std::string detail;
        for (int m = 0; m < (int) ExciterMode::numModes; ++m)
        {
            p.exc.mode = (ExciterMode) m;
            auto out = process (p, sine (1000.0, 1.0, 0.3f));
            const double fund = toneDb (out.l, 8000, 48000, 1000.0);
            const double h2 = toneDb (out.l, 8000, 48000, 2000.0), h3 = toneDb (out.l, 8000, 48000, 3000.0);
            const double harm = std::max (h2, h3) - fund;
            ok &= harm > -40.0 && std::abs (fund - 20.0 * std::log10 (0.3)) < 6.0;
            detail += num (harm, 0) + " ";
        }
        check (ok, "exciter modes add harmonics at a similar level (harmonics dBc: " + detail + ")");
    }

    // ---- Imager -----------------------------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.img.enabled = true;
        p.img.width = { 0.0f, 0.0f, 0.0f };
        auto wide = noise (1.0, 0.5f, 11);
        auto mono = process (p, wide);
        double diff = 0.0;
        for (size_t i = 4000; i < mono.size(); ++i) diff = std::max (diff, (double) std::abs (mono.l[i] - mono.r[i]));
        check (diff < 1.0e-4, "imager width 0 makes the output mono");

        EngineParams q = neutral();
        q.img.enabled = true;
        q.img.monoBassHz = 120.0f;
        auto bassSide = sine (50.0, 2.0, 0.3f, kPi);
        auto out = process (q, bassSide);
        check (dbRms (out.l, 48000, 96000) - dbRms (bassSide.l, 48000, 96000) < -20.0, "mono bass removes 50 Hz side content");
        auto hiSide = sine (2000.0, 1.0, 0.3f, kPi);
        auto outHi = process (q, hiSide);
        check (std::abs (dbRms (outHi.l, 24000, 48000) - dbRms (hiSide.l, 24000, 48000)) < 0.2, "mono bass keeps the side above the crossover");

        EngineParams st = neutral();
        st.img.enabled = true;
        auto monoIn = noise (1.0, 0.4f, 5);
        monoIn.r = monoIn.l;
        auto plain = process (st, monoIn);
        st.img.stereoize = 1.0f;
        auto stOut = process (st, monoIn);
        double sideE = 0.0, monoErr = 0.0;
        for (size_t i = 2000; i < stOut.size(); ++i)
        {
            sideE += std::pow (0.5 * (stOut.l[i] - stOut.r[i]), 2.0);
            monoErr = std::max (monoErr, std::abs (0.5 * (stOut.l[i] + stOut.r[i]) - 0.5 * (plain.l[i] + plain.r[i])));
        }
        check (sideE > 1.0 && monoErr < 1.0e-5, "stereoize widens a mono source and keeps the mono sum identical");
    }

    // ---- Stabilizer --------------------------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.stab.enabled = true;
        p.stab.amount = 1.0f;
        p.stab.speedMs = 300.0f;
        p.stab.target = normaliseCurve (BandCurve {}); // flat (pink) target
        auto bright = noise (6.0, 0.5f, 9, 3.0f);      // +3 dB / octave tilt
        auto out = process (p, bright);
        BandAnalyzer before, after;
        before.prepare (sr);
        after.prepare (sr);
        before.process (bright.l.data() + 144000, bright.r.data() + 144000, 144000);
        after.process (out.l.data() + 144000, out.r.data() + 144000, 144000);
        const auto b = before.getLevels (true), a = after.getLevels (true);
        const double tiltBefore = b[8] - b[3], tiltAfter = a[8] - a[3];
        check (tiltAfter < tiltBefore - 6.0, "stabilizer pulls a bright mix towards the target (8k vs 250 Hz: " + num (tiltBefore, 1) + " -> "
                                                 + num (tiltAfter, 1) + " dB)");
    }

    // ---- Maximizer -----------------------------------------------------------------------------------------------
    {
        bool okSample = true, okTrue = true;
        std::string detail;
        for (int m = 0; m < (int) MaxMode::numModes; ++m)
        {
            EngineParams p = neutral();
            p.max.enabled = true;
            p.max.gainDb = 12.0f;
            p.max.ceilingDb = -1.0f;
            p.max.mode = (MaxMode) m;
            p.max.truePeak = true;
            auto in = musicLike (4.0, 0.4f);
            const float before = integrated (in);
            auto out = process (p, in);
            const float pk = gainToDb (samplePeak (out)), tp = truePeakDb (out);
            okSample &= pk <= -1.0f + 1.0e-3f;
            okTrue &= tp < -0.9f;
            detail += num (integrated (out) - before, 1) + "/" + num (tp, 2) + " ";
        }
        check (okSample, "maximizer never exceeds the -1 dB ceiling (all modes)");
        check (okTrue, "true-peak mode keeps inter-sample peaks under the ceiling (+LU / dBTP: " + detail + ")");

        EngineParams p = neutral();
        p.max.enabled = true;
        p.max.gainDb = 6.0f;
        p.max.ceilingDb = 0.0f;
        auto quiet = sine (440.0, 1.0, 0.1f);
        auto out = process (p, quiet);
        check (std::abs (dbRms (out.l, 24000, 48000) - dbRms (quiet.l, 24000, 48000) - 6.0) < 0.05, "maximizer gain is clean below the ceiling");
    }

    // ---- Dither ----------------------------------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.dither = DitherMode::bits16;
        auto out = process (p, sine (1000.0, 0.5, 0.25f));
        bool onGrid = true;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const double v = out.l[i] * 32768.0;
            onGrid &= std::abs (v - std::round (v)) < 1.0e-3;
        }
        auto silence = process (p, sine (1000.0, 0.5, 0.0f));
        const double floorDb = dbRms (silence.l, 0, silence.size());
        check (onGrid && floorDb < -90.0 && floorDb > -105.0, "16-bit TPDF dither: output on the 16-bit grid, noise floor " + num (floorDb, 1) + " dBFS");
        p.dither = DitherMode::bits16Shaped;
        auto shaped = process (p, sine (1000.0, 0.5, 0.0f));
        // shaped noise is lower in the midrange than plain TPDF
        check (toneDb (shaped.l, 0, shaped.size(), 3000.0) < toneDb (silence.l, 0, silence.size(), 3000.0), "noise-shaped dither moves noise away from the midrange");
    }

    // ---- Bypass, gain match, delta, meters --------------------------------------------------------------------------
    {
        EngineParams p = neutral();
        p.max.enabled = true;
        p.max.gainDb = 8.0f;
        p.max.ceilingDb = -1.0f;
        auto in = musicLike (8.0, 0.3f);
        MasterEngine e;
        e.setParams (p);
        e.prepare (sr, 512);
        FrameCounter fc;
        Stereo processed = in;
        run (e, processed, 300, &fc);
        const float procLufs = integrated (processed, 96000);
        check (fc.frames >= 79 && fc.frames <= 81, "one loudness frame per 100 ms (" + std::to_string (fc.frames) + " in 8 s)");
        check (std::abs (e.getMeters().integrated - procLufs) < 0.3f, "engine integrated meter matches offline measurement (" + num (e.getMeters().integrated)
                                                                         + " / " + num (procLufs) + ")");
        check (e.getMeters().matchDb > 4.0f, "gain match tracks the loudness gain of the chain (" + num (e.getMeters().matchDb) + " dB)");

        p.bypass = true;
        p.gainMatch = true;
        e.setParams (p);
        Stereo byp = in;
        run (e, byp, 300);
        const float bypLufs = integrated (byp, 96000);
        check (std::abs (bypLufs - procLufs) < 1.0f, "gain-matched bypass plays the input at the master's loudness (" + num (bypLufs) + " vs " + num (procLufs) + " LUFS)");

        EngineParams d = neutral();
        d.delta = true;
        d.gainMatch = true;
        auto src = musicLike (6.0, 0.3f);
        auto delta = process (d, src);
        const double residual = dbRms (delta.l, 96000, delta.size()) - dbRms (src.l, 96000, src.size());
        check (residual < -60.0, "delta of a neutral chain is (almost) silent (" + num (residual, 1) + " dB)");

        EngineParams mono = neutral();
        mono.monitor = MonitorMode::side;
        auto sideOut = process (mono, sine (440.0, 0.5, 0.3f));
        check (dbRms (sideOut.l, 4000, sideOut.size()) < -100.0, "side monitor of a mono signal is silent");
    }

    // ---- Assistant ------------------------------------------------------------------------------------------------------
    {
        auto mix = musicLike (20.0, 0.25f, 3);
        EngineParams current;
        for (float targetLufs : { -14.0f, -9.0f })
        {
            const auto t0 = std::chrono::steady_clock::now();
            const auto res = runAssistant (mix.l.data(), mix.r.data(), (int) mix.size(), sr, current, targetCurve (TargetStyle::pop), targetLufs);
            const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            std::printf ("        assistant: %s (%.2f s)\n", res.summary.c_str(), secs);
            auto out = process (res.params, mix);
            const float lufs = integrated (out, (size_t) sr * 4);
            check (res.ok && std::abs (lufs - targetLufs) < 1.0f && truePeakDb (out) < -0.5f,
                   "assistant reaches " + num (targetLufs, 0) + " LUFS (" + num (lufs) + " LUFS, " + num (truePeakDb (out)) + " dBTP)");
        }
        auto silence = sine (100.0, 5.0, 0.0f);
        check (! runAssistant (silence.l.data(), silence.r.data(), (int) silence.size(), sr, current, targetCurve (TargetStyle::balanced), -14.0f).ok,
               "assistant refuses silence");

        // Reference analysis: a track compared with itself has no deviation
        const auto a = analyseTrack (mix.l.data(), mix.r.data(), (int) mix.size(), sr);
        check (a.valid && std::abs (a.integratedLufs - integrated (mix)) < 0.05f && std::abs (a.curve[2] + a.curve[3] + a.curve[4] + a.curve[5] + a.curve[6] + a.curve[7]) < 0.01f,
               "track analysis: loudness " + num (a.integratedLufs) + " LUFS and a normalised band curve");
    }

    // ---- Module order, sample rates, stress ------------------------------------------------------------------------------
    {
        check (orderFromString ("6543210")[0] == 6 && orderFromString ("0012345") == defaultModuleOrder() && orderFromString (nullptr) == defaultModuleOrder(),
               "module order strings are validated");

        std::mt19937 rng (5);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        bool finite = true, under = true;
        double worstMs = 0.0;
        for (int trial = 0; trial < 12; ++trial)
        {
            EngineParams p;
            for (auto& g : p.eq.gain) g = u (rng) * 24.0f - 12.0f;
            for (auto& m : p.eq.mode) m = (int) (u (rng) * 2.99f);
            p.eq.hpfHz = 30.0f;
            p.stab.enabled = p.dyneq.enabled = p.glue.enabled = p.mb.enabled = p.exc.enabled = p.img.enabled = true;
            p.stab.target = targetCurve ((TargetStyle) (trial % 8));
            p.stab.amount = 1.0f;
            for (auto& r : p.dyneq.range) r = u (rng) * 24.0f - 12.0f;
            p.glue.ratio = 10.0f;
            p.glue.makeupDb = 12.0f;
            p.mb.gainDb = { 12.0f, -6.0f, 12.0f };
            p.exc.mode = (ExciterMode) (trial % 5);
            p.exc.driveDb = { 24.0f, 24.0f, 24.0f };
            p.exc.mix = { 1.0f, 1.0f, 1.0f };
            p.img.width = { u (rng) * 2.0f, u (rng) * 2.0f, 2.0f };
            p.img.stereoize = 1.0f;
            p.img.monoBassHz = 150.0f;
            p.max.gainDb = 24.0f;
            p.max.mode = (MaxMode) (trial % 4);
            p.max.ceilingDb = -0.3f;
            p.max.link = u (rng);
            p.dither = (DitherMode) (trial % 4);
            ModuleOrder o = defaultModuleOrder();
            std::shuffle (o.begin(), o.end(), rng);
            p.order = o;
            const double rate = trial % 3 == 0 ? 44100.0 : (trial % 3 == 1 ? 96000.0 : 48000.0);
            auto in = musicLike (2.0, 0.9f, (unsigned) trial);
            const auto t0 = std::chrono::steady_clock::now();
            auto out = process (p, in, 333, rate);
            worstMs = std::max (worstMs, std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count());
            for (size_t i = 0; i < out.size(); ++i)
            {
                finite &= std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
                under &= std::abs (out.l[i]) <= 1.0f && std::abs (out.r[i]) <= 1.0f;
            }
        }
        check (finite && under, "12 random extreme full-chain settings (44.1/48/96 kHz, shuffled order) stay finite and under full scale");
        std::printf ("        slowest 2 s render with everything on: %.0f ms (%.0fx real time)\n", worstMs, 2000.0 / worstMs);
    }

    std::printf (failures == 0 ? "All engine tests passed\n" : "%d engine test(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
