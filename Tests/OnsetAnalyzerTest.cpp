// Onset analyzer tests. Signals are generated analytically (attack shape x carrier),
// so the true envelope-crossing times are known exactly and shifting t0 shifts the
// whole waveform by a fractional number of samples.
//
//  1. absolute placement vs. the analytic envelope (both envelope modes, many carriers / phases)
//  2. carrier-phase robustness: same envelope, different carrier start phase (what different
//     round robins / notes look like) must give the same onset
//  3. sub-sample shift tracking
//  4. latency subtraction, silence

#include "../Source/LegatoAnalyzer.h"
#include "../Source/OnsetAnalyzer.h"
#include "../Source/PresetModel.h"
#include "../Source/SineState.h"
#include <cstdio>

using namespace tp;

namespace
{
constexpr double fs = 48000.0;
constexpr double pi = juce::MathConstants<double>::pi;
int failures = 0;

void expect (bool ok, const juce::String& what, double got, double want, double tol)
{
    std::printf ("  %-52s got %+10.5f want %+10.5f (tol %.5f)  %s\n", what.toRawUTF8(), got, want, tol, ok ? "ok" : "FAIL");
    if (! ok)
        ++failures;
}

// A SINE Player component chunk: a VST2-style binary header ("VstW" ... "CcnK" byte counts ...) and the
// JSON after it. `headerByte` replaces one byte of the CcnK size field (0x7B = '{').
juce::MemoryBlock sineComponent (juce::uint8 headerByte, const char* extraHeader = "")
{
    const char* json = R"({"samplerVersion":"1.9","selectedEntry":{"instrumentIndex":"0"},"instruments":[{"title":"Test Trumpet",)"
                       R"("ksshift":"0","articulations":[{"id":"a1","title":"Staccato","enabled":"true","keyRangeLow":"48",)"
                       R"("keyRangeHigh":"84","mainOptions":{"switchValue":"24","modwheelControl":"false"},"roundRobinOptions":)"
                       R"({"resetAfterMS":"1500","randomRoundRobin":"false","roundRobins":[{"enabled":"true"},{"enabled":"true"}]},)"
                       R"("dynamicsOptions":{"dynamicLayers":[{"title":"p"},{"title":"f"}]}}]}]})";
    const juce::uint8 header[] = { 'V', 's', 't', 'W', 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0, 0,
                                   'C', 'c', 'n', 'K', 0, 1, headerByte, 0x62, 'F', 'P', 'C', 'h', 0, 0, 0, 2, 'Y', '3', '5', '5' };
    juce::MemoryBlock m (header, sizeof (header));
    m.append (extraHeader, std::strlen (extraHeader));
    m.append (json, std::strlen (json));
    return m;
}

// as juce::VST3PluginInstance::getStateInformation: <VST3PluginState><IComponent> in JUCE's binary XML ("VC2!")
juce::MemoryBlock pluginStateWith (const juce::MemoryBlock& component)
{
    juce::XmlElement xml ("VST3PluginState");
    xml.createNewChildElement ("IComponent")->addTextElement (component.toBase64Encoding());
    const auto text = xml.toString (juce::XmlElement::TextFormat().singleLine());
    juce::MemoryBlock out;
    out.append ("VC2!", 4);
    const auto len = juce::ByteOrder::swapIfBigEndian ((juce::uint32) text.getNumBytesAsUTF8());
    out.append (&len, 4);
    out.append (text.toRawUTF8(), text.getNumBytesAsUTF8() + 1);
    return out;
}

// Raised-cosine rise over riseMs, then exponential decay (tau = decayMs): a pluck/bow-like hit.
double envelopeAt (double tMs, double riseMs, double decayMs)
{
    if (tMs <= 0)
        return 0;
    if (tMs < riseMs)
        return 0.5 - 0.5 * std::cos (pi * tMs / riseMs);
    return std::exp (-(tMs - riseMs) / decayMs);
}

// Synthetic legato: note A sounds from the start; B's note-on is at sample `on`. Phase-continuous
// harmonic tone whose pitch follows the given path. Returns (withB, aloneA).
struct LegatoCase
{
    int a, b;
    enum Kind { slur, noisySlur, glide } kind = slur;
    double jumpMs = 80.0;      // slur: pitch jump; noisySlur: noise from jumpMs to jumpMs + noiseMs
    double noiseMs = 0.0;
    double glideStartMs = 20.0, glideMs = 150.0;
    double releaseDb = -200.0; // A keeps sounding under B at this level (SINE's release sample)
    double vibratoCents = 0.0; // on B
    int harmonics = 3;
};

std::pair<juce::AudioBuffer<float>, juce::AudioBuffer<float>> synthLegato (const LegatoCase& c, int on)
{
    const int N = on + (int) (1.0 * fs);
    juce::AudioBuffer<float> withB (2, N), alone (2, N);
    const double fA = midiToHz (c.a), fB = midiToHz (c.b);
    double phB = 0, phA = 0, phA2 = 0;
    juce::Random noise (7);
    auto tone = [&] (double ph) {
        double v = 0, norm = 0;
        for (int h = 1; h <= c.harmonics; ++h)
        {
            v += std::sin (h * ph) / h;
            norm += 1.0 / h;
        }
        return v / norm;
    };
    for (int n = 0; n < N; ++n)
    {
        const double tMs = (n - on) * 1000.0 / fs;
        // pitch path of the legato voice
        double hz = fA;
        if (c.kind == LegatoCase::glide)
        {
            const double g = juce::jlimit (0.0, 1.0, (tMs - c.glideStartMs) / c.glideMs);
            hz = fA * std::pow (fB / fA, g);
        }
        else if (tMs >= c.jumpMs + c.noiseMs)
            hz = fB;
        if (c.kind != LegatoCase::glide && tMs >= c.jumpMs + c.noiseMs && c.vibratoCents > 0)
            hz *= std::pow (2.0, c.vibratoCents * std::sin (2.0 * pi * 5.5 * tMs / 1000.0) / 1200.0);
        phB += 2.0 * pi * hz / fs;
        phA += 2.0 * pi * fA / fs;
        double v = tone (phB);
        if (c.kind == LegatoCase::noisySlur && tMs >= c.jumpMs && tMs < c.jumpMs + c.noiseMs)
            v = 0.7 * (noise.nextDouble() * 2.0 - 1.0);
        double rel = 0;
        if (c.releaseDb > -150.0 && tMs >= c.jumpMs)
        {
            phA2 += 2.0 * pi * fA / fs;
            rel = std::pow (10.0, c.releaseDb / 20.0) * tone (phA2 + phA - phA2) * std::exp (-(tMs - c.jumpMs) / 400.0);
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            withB.setSample (ch, n, (float) (0.25 * (v + rel)));
            alone.setSample (ch, n, (float) (0.25 * tone (phA)));
        }
    }
    return { withB, alone };
}

// time (ms after onset) where the rise reaches g (0..1) of its peak
double riseCrossingMs (double g, double riseMs) { return riseMs / pi * std::acos (1.0 - 2.0 * g); }

int gHarmonics = 1; // >1: carrier is a 1/k-weighted harmonic series (brass/string-like), each partial with its own phase

juce::AudioBuffer<float> makeHit (double onsetMs, double riseMs, double carrierHz, double phase, int pre,
                                  double decayMs = 300.0, double gain = 0.5, int channels = 2)
{
    const int N = pre + (int) (0.6 * fs);
    juce::AudioBuffer<float> b (channels, N);
    for (int n = 0; n < N; ++n)
    {
        const double tMs = (n - pre) * 1000.0 / fs - onsetMs;
        const double env = envelopeAt (tMs, riseMs, decayMs);
        double car = carrierHz > 0 ? 0.0 : 1.0;
        for (int h = 1; carrierHz > 0 && h <= gHarmonics; ++h)
            car += std::sin (2.0 * pi * carrierHz * h * tMs / 1000.0 + phase * (1.0 + 0.37 * (h - 1))) / h;
        for (int c = 0; c < channels; ++c)
            b.setSample (c, n, (float) (gain * env * car * (c == 0 ? 1.0 : 0.8)));
    }
    return b;
}

OnsetParams params (EnvelopeMode mode)
{
    OnsetParams p;
    p.envelope = mode;
    p.sampleRate = fs;
    p.preSamples = 480;
    p.peakWindowMs = 400;
    return p;
}

const char* modeName (EnvelopeMode m) { return m == EnvelopeMode::hilbert ? "hilbert" : "rms"; }

// --------------------------------------------------------------------------
double gPeriodSmoothing = 0.0;

void accuracyAndPhaseTest (EnvelopeMode mode, double tolAbsMs, double tolJitterMs, bool enforce, int harmonics = 1,
                           double periodSmoothing = 0.0)
{
    gHarmonics = harmonics;
    gPeriodSmoothing = periodSmoothing;
    std::printf ("\n[accuracy + phase robustness, %s envelope, %d harmonic(s), period smoothing %.2f]%s\n", modeName (mode),
                 harmonics, periodSmoothing, enforce ? "" : "  (informational)");
    const double onset = 5.0;
    for (double carrier : { 41.2, 110.0, 440.0, 2000.0 })
    {
        for (double rise : { 3.0, 15.0 })
        {
            for (int k = 0; k < 2; ++k)
            {
                const float thrDb = k == 0 ? -20.0f : -40.0f;
                const size_t ti = k == 0 ? 2 : 0;
                const double truth = onset + riseCrossingMs (std::pow (10.0, thrDb / 20.0), rise);
                double lo = 1e9, hi = -1e9, worst = 0;
                for (int ph = 0; ph < 8; ++ph)
                {
                    auto pp = params (mode);
                    pp.fundamentalHz = gPeriodSmoothing > 0 ? carrier : 0.0;
                    pp.periodSmoothing = gPeriodSmoothing;
                    auto r = analyseOnset (makeHit (onset, rise, carrier, ph * pi / 4.0, 480), pp);
                    lo = juce::jmin (lo, r.relMs[ti]);
                    hi = juce::jmax (hi, r.relMs[ti]);
                    worst = juce::jmax (worst, std::abs (r.relMs[ti] - truth));
                }
                auto label = juce::String (carrier, 1) + " Hz rise " + juce::String (rise, 0) + " ms rel" + juce::String ((int) thrDb);
                std::printf ("  %-34s truth %7.4f  worst err %7.4f ms  phase jitter %7.4f ms", label.toRawUTF8(), truth, worst, hi - lo);
                const bool ok = worst <= tolAbsMs && (hi - lo) <= tolJitterMs;
                std::printf ("  %s\n", enforce ? (ok ? "ok" : "FAIL") : "");
                if (enforce && ! ok)
                    ++failures;
            }
        }
    }
}

void shiftTest (double carrierHz, double tolSamples)
{
    std::printf ("\n[sub-sample shift tracking, hilbert, carrier %.0f Hz, rise 4 ms]\n", carrierHz);
    auto p = params (EnvelopeMode::hilbert);
    p.fundamentalHz = carrierHz;
    const auto base = analyseOnset (makeHit (5.0, 4.0, carrierHz, 0.3, p.preSamples), p);
    for (double frac : { 0.25, 0.5, 0.75, 10.3 })
    {
        const double shiftMs = frac * 1000.0 / fs;
        const auto r = analyseOnset (makeHit (5.0 + shiftMs, 4.0, carrierHz, 0.3, p.preSamples), p);
        for (auto& key : onsetMethodKeys())
        {
            // "first" is a noise-floor threshold: only sample-accurate by nature. Low notes: when the
            // attack lies inside the smoothing blur the rel crossings collapse onto "first".
            const bool collapsed = std::abs (r.get (key) - r.firstMs) < 1e-9; // crossing inside the smoothing blur
            // attackPeak is refined on a rounded corner: ~2-sample precision
            const double tol = (key == "attackPeak" ? 2.0 : key == "first" || collapsed || carrierHz < 100.0 ? 1.0 : tolSamples) * 1000.0 / fs;
            const double d = r.get (key) - base.get (key);
            expect (r.valid && std::abs (d - shiftMs) <= tol, "shift " + juce::String (frac, 2) + " smp, " + key, d, shiftMs, tol);
        }
    }
}
} // namespace

int main()
{
    {
        std::puts ("[silence]");
        juce::AudioBuffer<float> b (2, 48000);
        b.clear();
        auto r = analyseOnset (b, params (EnvelopeMode::hilbert));
        expect (! r.valid, "invalid on silence", r.valid, 0, 0);
    }
    {
        std::puts ("[latency subtraction, 64 samples]");
        auto p = params (EnvelopeMode::hilbert);
        auto a = analyseOnset (makeHit (5.0, 4.0, 440.0, 0.0, p.preSamples), p);
        p.latencySamples = 64;
        auto b = analyseOnset (makeHit (5.0 + 64 * 1000.0 / fs, 4.0, 440.0, 0.0, p.preSamples), p);
        expect (std::abs (a.get ("rel-20") - b.get ("rel-20")) < 1e-4, "rel-20 unchanged", b.get ("rel-20"), a.get ("rel-20"), 1e-4);
        expect (a.preRollDb < -150, "no pre-roll bleed reported", a.preRollDb, -200, 50);
    }
    {
        std::puts ("[peak level reads as amplitude]");
        auto r = analyseOnset (makeHit (5.0, 4.0, 1000.0, 0.0, 480, 1e9, 0.5, 1), params (EnvelopeMode::hilbert));
        expect (std::abs (r.peakDb - 20.0 * std::log10 (0.5)) < 0.1, "peak dB of 0.5 sine", r.peakDb, 20.0 * std::log10 (0.5), 0.1);
    }

    // informational comparison of the envelope variants
    for (int h : { 1, 12 })
        for (auto m : { EnvelopeMode::rms, EnvelopeMode::hilbert })
            for (double ps : { 0.0, 0.5, 1.0 })
                accuracyAndPhaseTest (m, 0, 0, false, h, ps);

    // enforced: pure tones in the mid/high register are exact with the raw Hilbert envelope
    gHarmonics = 1;
    gPeriodSmoothing = 0.0;
    std::puts ("\n[enforced: raw Hilbert, pure tones >= 440 Hz]");
    for (double carrier : { 440.0, 2000.0 })
        for (double rise : { 3.0, 15.0 })
        {
            double worst = 0, lo = 1e9, hi = -1e9;
            const double truth = 5.0 + riseCrossingMs (0.1, rise);
            for (int ph = 0; ph < 8; ++ph)
            {
                auto pp = params (EnvelopeMode::hilbert);
                pp.periodSmoothing = 0.0;
                auto r = analyseOnset (makeHit (5.0, rise, carrier, ph * pi / 4.0, 480), pp);
                worst = juce::jmax (worst, std::abs (r.get ("rel-20") - truth));
                lo = juce::jmin (lo, r.get ("rel-20"));
                hi = juce::jmax (hi, r.get ("rel-20"));
            }
            auto label = juce::String (carrier, 0) + " Hz rise " + juce::String (rise, 0) + " rel-20 ";
            expect (worst <= 0.02, label + "worst abs error", worst, 0, 0.02);
            expect (hi - lo <= 0.02, label + "phase jitter", hi - lo, 0, 0.02);
        }

    // enforced: default settings (0.5-period smoothing) stay phase-robust on harmonic-rich tones
    std::puts ("\n[enforced: default smoothing, 12 harmonics, phase jitter]");
    gHarmonics = 12;
    for (double carrier : { 110.0, 440.0 })
    {
        double lo = 1e9, hi = -1e9;
        for (int ph = 0; ph < 8; ++ph)
        {
            auto pp = params (EnvelopeMode::hilbert);
            pp.fundamentalHz = carrier;
            auto r = analyseOnset (makeHit (5.0, 3.0, carrier, ph * pi / 4.0, 480), pp);
            lo = juce::jmin (lo, r.get ("rel-20"));
            hi = juce::jmax (hi, r.get ("rel-20"));
        }
        expect (hi - lo <= 0.5 * 1000.0 / carrier / 2.0, juce::String (carrier, 0) + " Hz rel-20 jitter <= 1/4 period", hi - lo, 0,
                0.5 * 1000.0 / carrier / 2.0);
    }
    gHarmonics = 1;

    shiftTest (440.0, 0.1);
    shiftTest (55.0, 0.1);

    // ---- end of attack / start of decay
    {
        std::puts ("\n[attack end: pluck = raised-cosine rise then decay]");
        gHarmonics = 6;
        for (double carrier : { 110.0, 440.0, 1760.0 })
            for (double rise : { 5.0, 20.0, 60.0 })
            {
                auto pp = params (EnvelopeMode::hilbert);
                pp.fundamentalHz = carrier;
                pp.peakWindowMs = 400;
                double worstPeak = 0, worstEnd = 0;
                for (int ph = 0; ph < 4; ++ph)
                {
                    auto r = analyseOnset (makeHit (5.0, rise, carrier, ph * pi / 2.0, 480, 150.0), pp);
                    worstPeak = juce::jmax (worstPeak, std::abs (r.attackPeakMs - (5.0 + rise)));
                    worstEnd = juce::jmax (worstEnd, std::abs (r.attackEndMs - (5.0 + riseCrossingMs (std::pow (10.0, -1.0 / 20.0), rise))));
                }
                // tolerance: a few % of the rise, never tighter than ~1/4 period of the note
                const double tol = juce::jmax (0.05 * rise, 250.0 / carrier);
                auto label = juce::String (carrier, 0) + " Hz rise " + juce::String (rise, 0) + " ms ";
                // attackPeak sits on a sharp corner that full-period smoothing rounds off: ~1 period coarser
                const double tolPeak = juce::jmax (0.1 * rise, 1200.0 / carrier + 1.0);
                expect (worstPeak <= tolPeak, label + "attackPeak at end of rise", worstPeak, 0, tolPeak);
                expect (worstEnd <= tol, label + "attackEnd at -1 dB of rise", worstEnd, 0, tol);
            }
        gHarmonics = 1;
    }
    {
        std::puts ("\n[attack end: sustain = 30 ms attack to 50 %, then slow swell to 100 % over 800 ms]");
        const int pre = 480, N = pre + (int) (1.5 * fs);
        juce::AudioBuffer<float> b (2, N);
        for (int n = 0; n < N; ++n)
        {
            const double tMs = (n - pre) * 1000.0 / fs - 5.0;
            double env = 0;
            if (tMs > 0 && tMs < 30.0)
                env = 0.5 * (0.5 - 0.5 * std::cos (pi * tMs / 30.0));
            else if (tMs >= 30.0)
                env = 0.5 + 0.5 * juce::jmin (1.0, (tMs - 30.0) / 800.0);
            double car = 0;
            for (int h = 1; h <= 6; ++h)
                car += std::sin (2.0 * pi * 440.0 * h * tMs / 1000.0) / h;
            for (int c = 0; c < 2; ++c)
                b.setSample (c, n, (float) (0.3 * env * car));
        }
        auto pp = params (EnvelopeMode::hilbert);
        pp.fundamentalHz = 440.0;
        pp.peakWindowMs = 1200;
        auto r = analyseOnset (b, pp);
        // plateau rule: the attack is over once the next 40 ms add < 1 dB, i.e. already within 1 dB of the top
        expect (std::abs (r.attackPeakMs - 35.0) < 8.0, "attackPeak near end of the 30 ms attack, not the swell", r.attackPeakMs, 35.0, 8.0);
        const double endTruth = 5.0 + riseCrossingMs (std::pow (10.0, -1.0 / 20.0), 30.0);
        expect (std::abs (r.attackEndMs - endTruth) < 3.0, "attackEnd near -1 dB of the attack", r.attackEndMs, endTruth, 3.0);
        expect (r.peakMs > 500.0, "global 'peak' is the late swell (why attackPeak exists)", r.peakMs, 835.0, 400.0);
    }

    {
        std::puts ("\n[legato: abrupt slurs, noisy re-bows, glides, octaves, release under the new note, vibrato]");
        const int on = (int) (0.1 * fs);
        LegatoParams lp;
        lp.sampleRate = fs;
        const bool verbose = juce::SystemStats::getEnvironmentVariable ("VERBOSE", "0") == "1";
        double worstSlur = 0, worstStart = 0, worstGlide = 0, worstMid = 0, worstNoisy = 0;
        juce::String worstSlurCase, worstGlideCase;
        int invalid = 0;
        const auto onlyA = juce::SystemStats::getEnvironmentVariable ("ONLY_A", "0").getIntValue();
        const auto onlySemis = juce::SystemStats::getEnvironmentVariable ("ONLY_SEMIS", "0").getIntValue();
        // from the lowest orchestral note (contrabassoon Bb-1, 29 Hz) up; slurs are judged against a
        // tenth of the lower note's period below ~100 Hz (two periods are needed to see a pitch at all)
        for (int a : { 24, 36, 48, 60, 72, 84 })
            for (int semis : { 1, -1, 2, 5, -5, 7, 12, -12 })
                if ((onlyA == 0 || a == onlyA) && (onlySemis == 0 || semis == onlySemis) && a + semis >= 22)
            {
                const int b = a + semis;
                const double I = 100.0 * std::abs (semis);
                for (double jump : { 61.3, 83.77 })
                {
                    LegatoCase c { a, b };
                    c.jumpMs = jump;
                    auto [w, al] = synthLegato (c, on);
                    const auto r = analyseLegato (w, al, on, a, b, lp);
                    if (! r.valid) { ++invalid; std::printf ("    invalid %d->%d: %s\n", a, b, r.failure.toRawUTF8()); continue; }
                    if (verbose)
                        std::printf ("    slur %4s->%-4s jump %6.2f: start %+7.2f arrive %+7.2f\n", noteName (a).toRawUTF8(), noteName (b).toRawUTF8(),
                                     jump, r.startMs - jump, r.arriveMs - jump);
                    const double tol = juce::jmax (0.5, 0.1 * 1000.0 / midiToHz (juce::jmin (a, b)));
                    if (std::abs (r.arriveMs - jump) / tol > worstSlur)
                        worstSlur = std::abs (r.arriveMs - jump) / tol, worstSlurCase = noteName (a) + "->" + noteName (b);
                    worstStart = juce::jmax (worstStart, std::abs (r.startMs - jump) / tol);
                }
                {
                    LegatoCase c { a, b, LegatoCase::noisySlur };
                    c.jumpMs = 70.0;
                    c.noiseMs = 40.0;
                    c.releaseDb = -12.0;
                    auto [w, al] = synthLegato (c, on);
                    const auto r = analyseLegato (w, al, on, a, b, lp);
                    if (! r.valid) { ++invalid; std::printf ("    invalid noisy %d->%d: %s\n", a, b, r.failure.toRawUTF8()); continue; }
                    if (verbose)
                        std::printf ("    noisy %4s->%-4s: start %+7.2f arrive %+7.2f\n", noteName (a).toRawUTF8(), noteName (b).toRawUTF8(),
                                     r.startMs - 70.0, r.arriveMs - 110.0);
                    // a pitch needs ~2 periods to exist: below ~100 Hz the edges are judged in periods
                    const double per = 1000.0 / midiToHz (juce::jmin (a, b));
                    worstNoisy = juce::jmax (worstNoisy, juce::jmax (std::abs (r.arriveMs - 110.0) / juce::jmax (1.5, 0.15 * per),
                                                                     std::abs (r.startMs - 70.0) / juce::jmax (1.5, 0.4 * per)));
                }
                {
                    LegatoCase c { a, b, LegatoCase::glide };
                    auto [w, al] = synthLegato (c, on);
                    const auto r = analyseLegato (w, al, on, a, b, lp);
                    if (! r.valid) { ++invalid; std::printf ("    invalid glide %d->%d: %s\n", a, b, r.failure.toRawUTF8()); continue; }
                    const double band = juce::jmin (40.0, I / 3.0);
                    const double want = c.glideStartMs + c.glideMs * (1.0 - band / I);
                    if (verbose)
                        std::printf ("    glide %4s->%-4s: arrive %+7.2f mid %+7.2f start %7.2f\n", noteName (a).toRawUTF8(), noteName (b).toRawUTF8(),
                                     r.arriveMs - want, r.midMs - 95.0, r.startMs);
                    const double per = 1000.0 / midiToHz (juce::jmin (a, b));
                    const double tol = juce::jmax (1.0, (juce::jmin (a, b) < 36 ? 0.45 : 0.17) * per); // 33 Hz: ~2 periods = 60 ms per pitch estimate
                    if (std::abs (r.arriveMs - want) / tol > worstGlide)
                        worstGlide = std::abs (r.arriveMs - want) / tol, worstGlideCase = noteName (a) + "->" + noteName (b);
                    worstMid = juce::jmax (worstMid, std::abs (r.midMs - (c.glideStartMs + 0.5 * c.glideMs)) / tol);
                }
            }
        expect (invalid == 0, "every synthetic transition measured", invalid, 0, 0);
        expect (worstSlur <= 1.0, "slur arrival, error / max(0.5 ms, 0.1 period); worst " + worstSlurCase, worstSlur, 0, 1.0);
        expect (worstStart <= 1.0, "slur start = the same jump (same scale)", worstStart, 0, 1.0);
        expect (worstNoisy <= 1.0, "noisy re-bow + release at -12 dB: arrive / max(1.5 ms, 0.15 per), start / max(1.5, 0.4 per)", worstNoisy, 0, 1.0);
        expect (worstGlide <= 1.0, "glide enters the target band / max(1 ms, 0.17 per; C0: 0.45 per), worst " + worstGlideCase, worstGlide, 0, 1.0);
        expect (worstMid <= 1.0, "glide midpoint (same scale)", worstMid, 0, 1.0);

        // vibrato on the target (+-25 cents, 5.5 Hz): arrival still the jump
        double worstVib = 0;
        for (int a : { 48, 60, 72 })
            for (int semis : { 1, 2, -3, 7 })
            {
                LegatoCase c { a, a + semis };
                c.jumpMs = 75.0;
                c.vibratoCents = 25.0;
                auto [w, al] = synthLegato (c, on);
                const auto r = analyseLegato (w, al, on, a, a + semis, lp);
                worstVib = juce::jmax (worstVib, r.valid ? std::abs (r.arriveMs - 75.0) : 999.0);
            }
        expect (worstVib < 1.0, "slur onto a vibrato note (+-25 cents)", worstVib, 0, 1.0);
    }

    // ---- preset editing: range text and save -> load round trip
    {
        juce::Array<NoteRange> r;
        expect (parseRangeText ("C2-C6, D7", r) && r.size() == 2 && r[0].lo == 48 && r[0].hi == 96 && r[1].lo == 110 && r[1].hi == 110,
                "range text C2-C6, D7", r.size(), 2, 0);
        expect (parseRangeText ("C-1-C1", r) && r.size() == 1 && r[0].lo == 12 && r[0].hi == 36, "range text C-1-C1", r.size(), 1, 0);
        expect (parseRangeText ("36-96", r) && r.size() == 1 && r[0].lo == 36 && r[0].hi == 96, "range text 36-96", r.size(), 1, 0);
        expect (! parseRangeText ("C2-X9", r), "range text rejects C2-X9", 0, 0, 0);

        Collection c;
        c.name = "Round trip";
        c.player = "Kontakt";
        c.keyswitchBase = "C-1";
        Instrument inst;
        inst.name = "Violins";
        Articulation a;
        a.index = 3;
        a.name = "Spiccato";
        a.category = Category::shortNote;
        a.legato = true;
        a.ranges.add ({ 55, 96 });
        a.dynamics = { "p", "f" };
        a.roundRobins = 4;
        a.keyswitch = 13;
        a.dynamicsSource = DynamicsSource::cc1;
        inst.articulations.push_back (a);
        c.instruments.push_back (inst);
        const auto f = juce::File::createTempFile (".json");
        Collection back;
        const bool ok = c.save (f).wasOk() && Collection::load (f, back).wasOk() && back.instruments.size() == 1
                        && back.instruments[0].articulations.size() == 1;
        expect (ok, "preset save -> load", ok, 1, 0);
        if (ok)
        {
            const auto& b = back.instruments[0].articulations[0];
            expect (b.name == "Spiccato" && b.index == 3 && b.legato && b.roundRobins == 4 && b.keyswitch == 13
                        && b.dynamics == a.dynamics && b.ranges.size() == 1 && b.ranges[0].lo == 55 && b.ranges[0].hi == 96
                        && b.dynamicsSource == DynamicsSource::cc1 && back.keyswitchBase == "C-1" && back.player == "Kontakt",
                    "preset fields survive the round trip", 1, 1, 0);
        }
        f.deleteFile();
    }
    {
        // a loaded preset keeps the fields Secant doesn't model (legatoTypes, variations, ...) when saved again
        const auto f = juce::File::createTempFile (".json");
        f.replaceWithText (R"({"format":"secant.preset","version":1,"collection":"X","notes":"keep",
            "instruments":[{"name":"Vn","ext":1,"articulations":[{"index":1,"name":"Sus","category":"long",
            "ranges":[{"lo":48,"hi":96}],"dynamics":["p","f"],"roundRobins":1,"keyswitch":"C0",
            "legatoTypes":["Fingered","Slurred"],"tempoMeasured":true}]}]})");
        Collection c, back;
        bool ok = Collection::load (f, c).wasOk();
        if (ok)
        {
            c.instruments[0].articulations[0].keyswitch = -1;
            c.instruments[0].articulations[0].roundRobins = 3;
            ok = c.save (f).wasOk() && Collection::load (f, back).wasOk();
        }
        const auto j = juce::JSON::parse (f);
        const auto& av = j["instruments"][0]["articulations"][0];
        expect (ok && j["notes"] == juce::var ("keep") && (int) j["instruments"][0]["ext"] == 1
                    && av["legatoTypes"].size() == 2 && (bool) av["tempoMeasured"] && ! av.hasProperty ("keyswitch")
                    && (int) av["roundRobins"] == 3 && back.instruments[0].articulations[0].keyswitch == -1,
                "edited preset keeps unknown fields, drops a cleared keyswitch", ok, 1, 0);
        f.deleteFile();
    }

    // ---- SINE Player state: the JSON is found whatever bytes the binary header in front of it holds
    {
        auto parsedOk = [] (const juce::MemoryBlock& state) {
            const auto st = parseSineState (state);
            const auto* inst = st.instrument();
            return inst != nullptr && inst->title == "Test Trumpet" && inst->articulations.size() == 1
                   && inst->articulations[0].keyswitch == 24 && inst->articulations[0].roundRobins == 2
                   && juce::approximatelyEqual (inst->articulations[0].rrResetMs, 1500.0)
                   && inst->articulations[0].dynamics == juce::StringArray { "p", "f" };
        };
        for (const juce::uint8 b : { (juce::uint8) 0xd4, (juce::uint8) '{' })
        {
            const juce::String what = b == '{' ? "with a '{' byte in the header" : "plain header";
            const auto component = sineComponent (b);
            expect (parsedOk (component), "SINE component, " + what, parsedOk (component), 1, 0);
            const auto state = pluginStateWith (component);
            expect (parsedOk (state), "SINE plugin state, " + what, parsedOk (state), 1, 0);
            juce::String err;
            const auto patched = withRoundRobinReset (state, 900.0, &err);
            const auto patchedState = parseSineState (patched);
            const auto* inst = patchedState.instrument();
            const bool ok = inst != nullptr && juce::approximatelyEqual (inst->articulations[0].rrResetMs, 900.0)
                            && extractSineJson (patched).length() == extractSineJson (state).length();
            expect (ok, "SINE RR reset patch, " + what + (err.isEmpty() ? juce::String() : " (" + err + ")"), ok, 1, 0);
        }
        // a stray '{"' pair in the header (it opens a "string" that runs into the real JSON)
        const auto stray = sineComponent (0xd4, "{\"x");
        expect (parsedOk (stray), "SINE component, stray '{\"' in the header", parsedOk (stray), 1, 0);
        // binary that merely contains braces is not a SINE state
        const char notSine[] = "\x01{\x02{\"a\":1}\x7b\x7b\"instrumentsX\":2";
        expect (! parseSineState (juce::MemoryBlock (notSine, sizeof (notSine))).valid, "non-SINE binary with braces rejected", 0, 0, 0);
    }

    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
