#include "MasterEngine.h"

#include <cstring>

namespace lam
{
const char* moduleShortName (ModuleId m) noexcept
{
    switch (m)
    {
        case ModuleId::eq:         return "EQ";
        case ModuleId::stabilizer: return "STABILIZE";
        case ModuleId::dyneq:      return "DYN EQ";
        case ModuleId::glue:       return "GLUE";
        case ModuleId::dynamics:   return "DYNAMICS";
        case ModuleId::exciter:    return "EXCITER";
        case ModuleId::imager:     return "IMAGER";
        case ModuleId::numModules: break;
    }
    return "";
}

ModuleOrder defaultModuleOrder() noexcept
{
    return { (int) ModuleId::eq, (int) ModuleId::stabilizer, (int) ModuleId::dyneq, (int) ModuleId::glue,
             (int) ModuleId::dynamics, (int) ModuleId::exciter, (int) ModuleId::imager };
}

ModuleOrder orderFromString (const char* text) noexcept
{
    ModuleOrder order {};
    if (text == nullptr || std::strlen (text) != (size_t) kNumOrderable)
        return defaultModuleOrder();
    bool seen[kNumOrderable] {};
    for (int i = 0; i < kNumOrderable; ++i)
    {
        const int v = text[i] - '0';
        if (v < 0 || v >= kNumOrderable || seen[v])
            return defaultModuleOrder();
        seen[v] = true;
        order[(size_t) i] = v;
    }
    return order;
}

void orderToString (const ModuleOrder& order, char* out) noexcept
{
    for (int i = 0; i < kNumOrderable; ++i)
        out[i] = (char) ('0' + order[(size_t) i]);
    out[kNumOrderable] = 0;
}

//==============================================================================
void MasterEngine::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    maxBlock = std::max (16, maxBlockSize);

    eq.prepare (sr);
    stab.prepare (sr);
    dyneq.prepare (sr);
    glue.prepare (sr);
    mb.prepare (sr);
    exc.prepare (sr);
    img.prepare (sr);
    maximizer.prepare (sr);
    inMeter.prepare (sr);
    outMeter.prepare (sr);

    for (auto* v : { &rawL, &rawR, &workL, &workR, &dryL, &dryR, &analysisIn, &analysisOut, &scopeL, &scopeR })
        v->assign ((size_t) maxBlock, 0.0f);

    totalLatency = ExciterModule::getLatency() + maximizer.getLatency();
    for (auto& d : dryDelay)
    {
        d.prepare (totalLatency + 8);
        d.setDelay (totalLatency);
    }

    for (auto* s : { &inGain, &outGain, &matchSm })
        s->setTime (sr, 20.0f);
    for (auto* s : { &balanceSm, &bypassSm, &deltaSm })
        s->setTime (sr, 25.0f);
    lastSubsonic = -1.0f;
    setParams (params);
    reset();
}

void MasterEngine::reset()
{
    for (auto& f : subsonic) f.reset();
    eq.reset();
    stab.reset();
    dyneq.reset();
    glue.reset();
    mb.reset();
    exc.reset();
    img.reset();
    maximizer.reset();
    dither.reset();
    inMeter.reset();
    outMeter.reset();
    for (auto& t : outTp) t.reset();
    for (auto& d : dryDelay) d.reset();
    hopTruePeak = 0.0f;
    corrLR = corrLL = corrRR = 0.0;
    matchDb = 0.0f;
    inGain.snap (dbToGain (params.inputGainDb));
    outGain.snap (dbToGain (params.outputGainDb));
    balanceSm.snap (params.balance);
    bypassSm.snap (params.bypass ? 1.0f : 0.0f);
    deltaSm.snap (params.delta ? 1.0f : 0.0f);
    matchSm.snap (1.0f);
    meters = {};
}

void MasterEngine::resetLoudness() noexcept
{
    inMeter.resetIntegrated();
    outMeter.resetIntegrated();
    meters.truePeakMaxDb = -100.0f;
}

void MasterEngine::setParams (const EngineParams& p) noexcept
{
    params = p;
    if (p.subsonicHz != lastSubsonic)
    {
        for (auto& f : subsonic) f.setHighPass (sr, std::max (10.0f, p.subsonicHz));
        lastSubsonic = p.subsonicHz;
    }
    eq.setParams (p.eq);
    stab.setParams (p.stab);
    dyneq.setParams (p.dyneq);
    glue.setParams (p.glue);
    mb.setParams (p.mb);
    exc.setParams (p.exc);
    img.setParams (p.img);
    maximizer.setParams (p.max);
}

void MasterEngine::runModule (ModuleId id, float* l, float* r, int n) noexcept
{
    switch (id)
    {
        case ModuleId::eq:         if (params.eq.enabled) eq.process (l, r, n); break;
        case ModuleId::stabilizer: if (params.stab.enabled) stab.process (l, r, n); break;
        case ModuleId::dyneq:      if (params.dyneq.enabled) dyneq.process (l, r, n); break;
        case ModuleId::glue:       if (params.glue.enabled) glue.process (l, r, n); break;
        case ModuleId::dynamics:   if (params.mb.enabled) mb.process (l, r, n); break;
        case ModuleId::exciter:    exc.process (l, r, n); break; // always runs: constant latency
        case ModuleId::imager:     if (params.img.enabled) img.process (l, r, n); break;
        case ModuleId::numModules: break;
    }
}

void MasterEngine::process (float* const* io, int channels, int n, LoudnessListener* listener) noexcept
{
    n = std::min (n, maxBlock);
    float* inL = io[0];
    float* inR = channels > 1 ? io[1] : io[0];
    float* l = workL.data();
    float* r = workR.data();

    // ---- Input -------------------------------------------------------------------------------------
    const float ig = dbToGain (params.inputGainDb);
    const bool subOn = params.subsonicHz > 10.5f;
    float inPeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = inL[i], b = inR[i];
        rawL[(size_t) i] = a;
        rawR[(size_t) i] = b;
        inPeak = std::max (inPeak, std::max (std::abs (a), std::abs (b)));
        dryL[(size_t) i] = dryDelay[0].process (a);
        dryR[(size_t) i] = dryDelay[1].process (b);
        analysisIn[(size_t) i] = 0.5f * (a + b);
        const float g = inGain.next (ig);
        const float bal = balanceSm.next (clampf (params.balance, -1.0f, 1.0f));
        float x = a * g * std::min (1.0f, 1.0f - bal), y = b * g * std::min (1.0f, 1.0f + bal);
        if (subOn)
        {
            x = subsonic[0].process (x);
            y = subsonic[1].process (y);
        }
        l[i] = x;
        r[i] = y;
    }

    // ---- Chain ---------------------------------------------------------------------------------------
    for (int idx : params.order)
        runModule ((ModuleId) idx, l, r, n);
    maximizer.process (l, r, n);

    const float og = dbToGain (params.outputGainDb);
    for (int i = 0; i < n; ++i)
    {
        const float g = outGain.next (og);
        l[i] *= g;
        r[i] *= g;
    }

    // ---- Metering (hop aligned so every 100 ms frame is reported) ------------------------------------
    float outPeak = 0.0f;
    for (int pos = 0; pos < n;)
    {
        const int k = std::min (n - pos, outMeter.samplesToNextHop());
        for (int i = pos; i < pos + k; ++i)
        {
            const float tp = std::max (outTp[0].process (l[i]), outTp[1].process (r[i]));
            hopTruePeak = std::max (hopTruePeak, tp);
            outPeak = std::max (outPeak, std::max (std::abs (l[i]), std::abs (r[i])));
            corrLR += (double) l[i] * r[i];
            corrLL += (double) l[i] * l[i];
            corrRR += (double) r[i] * r[i];
        }
        inMeter.process (rawL.data() + pos, rawR.data() + pos, k);
        if (outMeter.process (l + pos, r + pos, k))
        {
            const float inSt = inMeter.getShortTerm(), outSt = outMeter.getShortTerm();
            if (inSt > -60.0f && outSt > -60.0f)
                matchDb += 0.2f * (clampf (outSt - inSt, -30.0f, 30.0f) - matchDb);

            const float tpDb = gainToDb (hopTruePeak);
            meters.truePeakMaxDb = std::max (meters.truePeakMaxDb, tpDb);
            if (listener != nullptr)
            {
                LoudnessFrame f;
                f.momentary = outMeter.getMomentary();
                f.shortTerm = outSt;
                f.inShortTerm = inSt;
                f.maxGrDb = maximizer.getReductionDb();
                f.truePeakDb = tpDb;
                listener->loudnessFrame (f);
            }
            hopTruePeak = 0.0f;

            // Correlation over the last ~100 ms, lightly smoothed
            const double den = std::sqrt (corrLL * corrRR);
            const float c = den > 1.0e-9 ? (float) (corrLR / den) : 1.0f;
            meters.correlation += 0.5f * (c - meters.correlation);
            corrLR = corrLL = corrRR = 0.0;
        }
        pos += k;
    }

    for (int i = 0; i < n; ++i)
    {
        analysisOut[(size_t) i] = 0.5f * (l[i] + r[i]);
        scopeL[(size_t) i] = l[i];
        scopeR[(size_t) i] = r[i];
    }

    // ---- Gain-matched bypass / delta -----------------------------------------------------------------
    const float matchTarget = dbToGain (matchDb);
    const bool anyCompare = params.bypass || params.delta || bypassSm.value > 1.0e-4f || deltaSm.value > 1.0e-4f;
    if (anyCompare)
    {
        for (int i = 0; i < n; ++i)
        {
            const float mg = matchSm.next (matchTarget);
            const float bypassGain = params.gainMatch ? mg : 1.0f;
            const float b = bypassSm.next (params.bypass ? 1.0f : 0.0f);
            const float d = deltaSm.next (params.delta ? 1.0f : 0.0f);
            auto safe = [] (float x)
            {
                // the level-matched input can be louder than full scale: round it off softly
                const float ax = std::abs (x);
                if (ax <= 0.9f) return x;
                const float y = 0.9f + 0.1f * std::tanh ((ax - 0.9f) * 10.0f);
                return x < 0.0f ? -y : y;
            };
            for (int ch = 0; ch < 2; ++ch)
            {
                float* p = ch == 0 ? l : r;
                const float dry = (ch == 0 ? dryL : dryR)[(size_t) i];
                float y = p[i] + b * (safe (dry * bypassGain) - p[i]);
                y += d * ((p[i] - dry * mg) - y);
                p[i] = y;
            }
        }
    }
    else
        matchSm.snap (matchTarget);

    // ---- Dither and monitoring -------------------------------------------------------------------------
    dither.process (l, r, n, params.dither);

    for (int i = 0; i < n; ++i)
    {
        float a = std::isfinite (l[i]) ? l[i] : 0.0f;
        float b = std::isfinite (r[i]) ? r[i] : 0.0f;
        switch (params.monitor)
        {
            case MonitorMode::mono:  a = b = 0.5f * (a + b); break;
            case MonitorMode::side:  { const float s = 0.5f * (a - b); a = s; b = s; break; }
            case MonitorMode::left:  b = a; break;
            case MonitorMode::right: a = b; break;
            case MonitorMode::stereo: case MonitorMode::numModes: break;
        }
        if (channels > 1)
        {
            inL[i] = a;
            inR[i] = b;
        }
        else
            inL[i] = 0.5f * (a + b);
    }

    // ---- Meters ------------------------------------------------------------------------------------------
    meters.inPeak = inPeak;
    meters.outPeak = outPeak;
    meters.momentary = outMeter.getMomentary();
    meters.shortTerm = outMeter.getShortTerm();
    meters.integrated = outMeter.getIntegrated();
    meters.range = outMeter.getRange();
    meters.inShortTerm = inMeter.getShortTerm();
    meters.inIntegrated = inMeter.getIntegrated();
    meters.glueDb = params.glue.enabled ? glue.getReductionDb() : 0.0f;
    meters.maxDb = params.max.enabled ? maximizer.getReductionDb() : 0.0f;
    meters.mbDb = params.mb.enabled ? mb.getReductionDb() : std::array<float, kMbBands> {};
    meters.dynEqDb = params.dyneq.enabled ? dyneq.getGains() : std::array<float, kDynEqBands> {};
    meters.stab = params.stab.enabled ? stab.getGains() : BandCurve {};
    meters.matchDb = matchDb;
}
} // namespace lam
