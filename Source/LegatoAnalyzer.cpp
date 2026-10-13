#include "LegatoAnalyzer.h"
#include "OnsetAnalyzer.h"
#include <juce_dsp/juce_dsp.h>
#include <map>

namespace tp
{
namespace
{
    juce::dsp::FFT& fftFor (int order)
    {
        thread_local std::map<int, std::unique_ptr<juce::dsp::FFT>> ffts;
        auto& f = ffts[order];
        if (f == nullptr)
            f = std::make_unique<juce::dsp::FFT> (order);
        return *f;
    }

    double median (std::vector<double> v)
    {
        if (v.empty())
            return 0.0;
        const auto mid = v.begin() + (long) (v.size() / 2);
        std::nth_element (v.begin(), mid, v.end());
        return *mid;
    }

    double rmsOf (const juce::AudioBuffer<float>& b, int start, int len)
    {
        start = juce::jlimit (0, b.getNumSamples(), start);
        len = juce::jlimit (0, b.getNumSamples() - start, len);
        if (len <= 0)
            return 0.0;
        double s = 0;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* x = b.getReadPointer (c, start);
            for (int n = 0; n < len; ++n)
                s += (double) x[n] * x[n];
        }
        return std::sqrt (s / len / juce::jmax (1, b.getNumChannels()));
    }
} // namespace

std::vector<PitchFrame> trackPitch (const juce::AudioBuffer<float>& buffer, int start, int numSamples, int zeroSample,
                                    const PitchTrackParams& p)
{
    std::vector<PitchFrame> frames;
    const double fs = p.sampleRate;
    // probed periods may lie outside the pick range (+ tolerance)
    double lowest = p.minHz;
    for (auto hz : p.probeHz)
        if (hz > 0)
            lowest = juce::jmin (lowest, hz * std::pow (2.0, -p.probeCents / 1200.0));
    const int maxTau = (int) std::ceil (fs / lowest) + 2;
    const int pickMaxTau = juce::jmin (maxTau - 1, (int) std::ceil (fs / p.minHz) + 1);
    const int minTau = juce::jmax (2, (int) std::floor (fs / p.maxHz));
    const int W = juce::jmax (32, (int) std::ceil (p.windowPeriods * fs / p.minHz));
    const int hop = juce::jmax (1, (int) std::round (p.hopMs * fs / 1000.0));

    start = juce::jmax (0, start);
    const int end = juce::jmin (buffer.getNumSamples(), start + numSamples);
    if (end - start < W + maxTau)
        return frames;

    std::vector<double> x ((size_t) (end - start), 0.0);
    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        auto* src = buffer.getReadPointer (c, start);
        for (size_t n = 0; n < x.size(); ++n)
            x[n] += src[n];
    }
    std::vector<double> prefix (x.size() + 1, 0.0);
    for (size_t n = 0; n < x.size(); ++n)
        prefix[n + 1] = prefix[n] + x[n] * x[n];

    int order = 1;
    while ((1 << order) < W + maxTau)
        ++order;
    const int N = 1 << order;
    auto& fft = fftFor (order);
    std::vector<juce::dsp::Complex<float>> a ((size_t) N), b ((size_t) N), A ((size_t) N), B ((size_t) N);
    std::vector<double> dn ((size_t) maxTau + 1);
    const double silence = std::pow (10.0, p.silenceDb / 20.0);

    // probe windows in samples of lag
    int probeLo[2] {}, probeHi[2] {};
    for (int i = 0; i < 2; ++i)
        if (p.probeHz[i] > 0)
        {
            probeLo[i] = juce::jmax (1, (int) std::floor (fs / (p.probeHz[i] * std::pow (2.0, p.probeCents / 1200.0))));
            probeHi[i] = juce::jmin (maxTau, (int) std::ceil (fs / (p.probeHz[i] * std::pow (2.0, -p.probeCents / 1200.0))));
        }

    for (int s = 0; s + W + maxTau <= (int) x.size(); s += hop)
    {
        PitchFrame f;
        const double e0 = prefix[(size_t) (s + W)] - prefix[(size_t) s];
        const double rms = std::sqrt (e0 / W / juce::jmax (1, buffer.getNumChannels()));
        f.rmsDb = juce::Decibels::gainToDecibels (rms, -200.0);
        f.timeMs = (start + s + W * 0.5 - zeroSample) * 1000.0 / fs;
        if (rms < silence)
        {
            frames.push_back (f);
            continue;
        }
        for (int n = 0; n < N; ++n)
        {
            a[(size_t) n] = { n < W ? (float) x[(size_t) (s + n)] : 0.0f, 0.0f };
            b[(size_t) n] = { n < W + maxTau ? (float) x[(size_t) (s + n)] : 0.0f, 0.0f };
        }
        fft.perform (a.data(), A.data(), false);
        fft.perform (b.data(), B.data(), false);
        for (int k = 0; k < N; ++k)
            A[(size_t) k] = std::conj (A[(size_t) k]) * B[(size_t) k];
        fft.perform (A.data(), a.data(), true); // a[tau] = sum_j x[s+j] x[s+j+tau]

        // cumulative-mean-normalised difference d'(tau)
        double running = 0;
        dn[0] = 1.0;
        for (int tau = 1; tau <= maxTau; ++tau)
        {
            const double et = prefix[(size_t) (s + tau + W)] - prefix[(size_t) (s + tau)];
            const double d = juce::jmax (0.0, e0 + et - 2.0 * (double) a[(size_t) tau].real());
            running += d;
            dn[(size_t) tau] = running > 0 ? d * tau / running : 1.0;
        }
        for (int i = 0; i < 2; ++i)
            if (p.probeHz[i] > 0)
                for (int tau = probeLo[i]; tau <= probeHi[i]; ++tau)
                    f.probe[i] = juce::jmin (f.probe[i], dn[(size_t) tau]);

        // pitch pick: the global minimum, then the smallest lag whose dip is nearly as deep
        // (a period's multiples are dips too - taking the deepest alone gives octave-down errors)
        int best = minTau;
        for (int tau = minTau; tau <= pickMaxTau; ++tau)
            if (dn[(size_t) tau] < dn[(size_t) best])
                best = tau;
        const double accept = juce::jmax (p.yinThreshold, dn[(size_t) best] + 0.1);
        for (int tau = minTau; tau < best; ++tau)
            if (dn[(size_t) tau] < accept && dn[(size_t) tau] <= dn[(size_t) tau - 1] && dn[(size_t) tau] <= dn[(size_t) tau + 1])
            {
                best = tau;
                break;
            }
        double tau = best;
        if (best > 1 && best + 1 <= maxTau)
        {
            const double y0 = dn[(size_t) best - 1], y1 = dn[(size_t) best], y2 = dn[(size_t) best + 1];
            const double den = y0 - 2.0 * y1 + y2;
            if (std::abs (den) > 1e-12)
                tau += juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den);
        }
        f.aperiodicity = dn[(size_t) best];
        f.timeMs = (start + s + (W + tau) * 0.5 - zeroSample) * 1000.0 / fs;
        if (f.aperiodicity <= p.voicedMaxAperiodicity)
            f.hz = fs / tau;
        frames.push_back (f);
    }
    return frames;
}

juce::StringArray legatoMethodKeys() { return { "arrive", "mid", "start", "diverge", "diffAttackEnd" }; }

double LegatoResult::get (const juce::String& m) const
{
    if (m == "arrive") return arriveMs;
    if (m == "mid") return midMs;
    if (m == "start") return startMs;
    if (m == "diverge") return divergeMs;
    if (m == "diffAttackEnd") return diffAttackEndMs;
    return std::numeric_limits<double>::quiet_NaN();
}

LegatoResult analyseLegato (const juce::AudioBuffer<float>& withB, const juce::AudioBuffer<float>& aloneA, int t0, int sourceNote,
                            int targetNote, const LegatoParams& p)
{
    LegatoResult r;
    const double fs = p.sampleRate;
    const int n = juce::jmin (withB.getNumSamples(), aloneA.getNumSamples());
    const int chans = juce::jmin (withB.getNumChannels(), aloneA.getNumChannels());
    auto toMs = [&] (double sample) { return (sample - t0 - p.latencySamples) * 1000.0 / fs; };
    auto ms = [&] (double v) { return (int) std::round (v * fs / 1000.0); };

    const int stableFrom = juce::jmin (n - 1, t0 + p.latencySamples + ms (p.stableFromMs));
    const int stableTo = juce::jmin (n, t0 + p.latencySamples + ms (p.stableToMs));
    const double steady = rmsOf (withB, stableFrom, stableTo - stableFrom);
    r.steadyDb = juce::Decibels::gainToDecibels (steady, -200.0);

    // --- difference to "A alone": exact, since the plugin renders deterministically from the same state
    juce::AudioBuffer<float> diff (chans, n);
    for (int c = 0; c < chans; ++c)
    {
        diff.copyFrom (c, 0, withB, c, 0, n);
        diff.addFrom (c, 0, aloneA, c, 0, n, -1.0f);
    }
    {
        const float thr = (float) (steady * std::pow (10.0, p.divergeDb / 20.0));
        for (int i = 0; i < n && thr > 0; ++i)
        {
            float m = 0;
            for (int c = 0; c < chans; ++c)
                m = juce::jmax (m, std::abs (diff.getSample (c, i)));
            if (m > thr)
            {
                r.divergeMs = toMs (i);
                r.diffValid = true;
                break;
            }
        }
        if (r.diffValid)
        {
            OnsetParams op;
            op.sampleRate = fs;
            op.preSamples = t0;
            op.latencySamples = p.latencySamples;
            op.peakWindowMs = juce::jmin (p.stableFromMs, (n - t0) * 1000.0 / fs);
            op.fundamentalHz = midiToHz (targetNote);
            const auto o = analyseOnset (diff, op);
            if (o.valid)
                r.diffAttackEndMs = o.attackEndMs;
            else
                r.diffValid = false;
        }
    }

    // --- pitch
    if (sourceNote == targetNote)
    {
        r.failure = "unison: no pitch change (use diffAttackEnd)";
        return r;
    }
    const double fA = midiToHz (sourceNote), fB = midiToHz (targetNote);
    const double interval = 100.0 * (targetNote - sourceNote); // cents
    const double absI = std::abs (interval);
    const bool octaves = (std::abs (targetNote - sourceNote) % 12) == 0;
    const bool targetIsHigher = targetNote > sourceNote;

    PitchTrackParams pp;
    pp.sampleRate = fs;
    pp.minHz = juce::jmin (fA, fB) / 1.3;
    pp.maxHz = juce::jmax (fA, fB) * 1.3;
    pp.windowPeriods = 2.0;
    pp.voicedMaxAperiodicity = 0.5; // mixed windows at an abrupt change still get a pitch (the dominant note's)
    pp.probeHz[0] = fA;
    pp.probeHz[1] = fB;
    pp.probeCents = juce::jmin (p.noteCents, absI / 3.0);
    const int from = juce::jmax (0, t0 - ms (60));
    const auto frames = trackPitch (withB, from, stableTo - from, t0 + p.latencySamples, pp);
    if (frames.empty())
    {
        r.failure = "capture too short for pitch tracking";
        return r;
    }

    // steady pitch of B (and of A before the note-on): centres the note bands on the recording's
    // real intonation and widens them for vibrato
    auto centsTo = [] (double hz, double ref) { return 1200.0 * std::log2 (hz / ref); };
    auto steadyStats = [&] (double ref, double fromT, double toT, double& centre, double& mad) {
        std::vector<double> c;
        for (auto& f : frames)
            if (f.timeMs >= fromT && f.timeMs < toT && f.hz > 0 && f.aperiodicity <= 0.35)
                if (const double x = centsTo (f.hz, ref); std::abs (x) < 100.0)
                    c.push_back (x);
        if (c.size() < 10)
            return false;
        centre = median (c);
        std::vector<double> dev;
        for (auto x : c)
            dev.push_back (std::abs (x - centre));
        mad = median (dev);
        return true;
    };
    double centreT = 0, madT = 0, centreS = 0, madS = 0;
    if (! steadyStats (fB, p.stableFromMs, p.stableToMs + 1.0, centreT, madT))
    {
        r.failure = "target note not established in the steady state (dropped note or no legato)";
        return r;
    }
    steadyStats (fA, -1.0e9, -5.0, centreS, madS);
    r.targetCentsError = centreT;
    r.vibratoCents = madT;
    const double base = juce::jmin (p.noteCents, absI / 3.0);
    const double bandT = juce::jmin (absI / 2.5, juce::jmax (base, 2.5 * madT + 5.0));
    const double bandS = juce::jmin (absI / 2.5, juce::jmax (base, 2.5 * madS + 5.0));

    // per-frame state from the pitch pick: whichever note dominates the window wins, so an abrupt
    // change flips half way through the window (no window-length bias); in-between pitches = glide
    enum State { none = 0, source = 1, target = 2 };
    std::vector<int> state (frames.size(), none);
    std::vector<double> cT (frames.size(), 1.0e9), cS (frames.size(), 1.0e9);
    // Octaves: how well the higher note's period fits each note on its own decides the boundary. A real
    // string tone with strong even harmonics fits half its own period quite well, so no fixed threshold works:
    // take the middle between the source alone (before the note-on) and the target alone (steady state).
    double octaveFrameBoundary = p.octaveRejectDprime;
    if (octaves)
    {
        const int hi = targetIsHigher ? 1 : 0;
        std::vector<double> dS, dT;
        for (auto& f : frames)
            if (f.rmsDb > -70.0)
            {
                if (f.timeMs < -5.0)
                    dS.push_back (f.probe[hi]);
                else if (f.timeMs >= p.stableFromMs)
                    dT.push_back (f.probe[hi]);
            }
        if (dS.size() >= 10 && dT.size() >= 10)
            octaveFrameBoundary = 0.5 * (median (dS) + median (dT));
    }
    for (size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i].hz <= 0)
            continue;
        double hz = frames[i].hz;
        if (octaves)
        {
            // a pick on the lower note while the higher note's period also fits = the higher note
            // (a higher-note signal is periodic in the lower period too)
            const double fHigh = juce::jmax (fA, fB), fLow = juce::jmin (fA, fB);
            const double dHigh = targetIsHigher ? frames[i].probe[1] : frames[i].probe[0];
            if (std::abs (centsTo (hz, fLow)) < 100.0 && dHigh < octaveFrameBoundary)
                hz = fHigh * (hz / fLow);
        }
        cT[i] = centsTo (hz, fB) - centreT;
        cS[i] = centsTo (hz, fA) - centreS;
        if (std::abs (cT[i]) <= bandT)
            state[i] = target;
        else if (std::abs (cS[i]) <= bandS)
            state[i] = source;
    }
    // runs shorter than minRunMs are noise: drop them
    {
        size_t i = 0;
        while (i < frames.size())
        {
            size_t j = i;
            while (j + 1 < frames.size() && state[j + 1] == state[i])
                ++j;
            if (state[i] != none && frames[j].timeMs - frames[i].timeMs < p.minRunMs)
                for (size_t k = i; k <= j; ++k)
                    state[k] = none;
            i = j + 1;
        }
    }

    if (juce::SystemStats::getEnvironmentVariable ("LEGATO_DEBUG", "0") == "2")
        for (size_t i = 0; i < frames.size(); i += 10)
            std::printf ("    %7.1f  %7.1f Hz  ap %.2f dS %.2f dT %.2f  %d\n", frames[i].timeMs, frames[i].hz, frames[i].aperiodicity,
                         frames[i].probe[0], frames[i].probe[1], state[i]);
    if (juce::SystemStats::getEnvironmentVariable ("LEGATO_DEBUG", "0") == "1")
        for (size_t i = 0, j; i < frames.size(); i = j + 1)
        {
            for (j = i; j + 1 < frames.size() && state[j + 1] == state[i];)
                ++j;
            std::printf ("    [%7.1f .. %7.1f] %s\n", frames[i].timeMs, frames[j].timeMs, state[i] == source ? "SOURCE" : state[i] == target ? "target" : "-");
        }

    // In a fast passage the previous transition may not have arrived when this note-on comes: then the
    // "source" never sounded and the target pitch is already there - there is no delay to measure
    {
        int voicedPre = 0, targetPre = 0;
        for (size_t i = 0; i < frames.size(); ++i)
            if (frames[i].timeMs >= -50.0 && frames[i].timeMs < -2.0 && state[i] != none)
            {
                ++voicedPre;
                targetPre += state[i] == target ? 1 : 0;
            }
        if (voicedPre >= 10 && targetPre * 2 > voicedPre)
        {
            r.failure = "source not sounding at the note-on: the target pitch was already there (the previous "
                        "transition had not arrived yet at this note spacing)";
            return r;
        }
    }
    {
        int steadyFrames = 0, steadyTarget = 0;
        for (size_t i = 0; i < frames.size(); ++i)
            if (frames[i].timeMs >= p.stableFromMs)
            {
                ++steadyFrames;
                steadyTarget += state[i] == target ? 1 : 0;
            }
        if (steadyFrames < 10 || steadyTarget < steadyFrames / 2)
        {
            r.failure = "target note not established in the steady state (dropped note or no legato)";
            return r;
        }
    }

    // arrival: first target frame after the last source frame (from note-on on), confirmed by the
    // target holding most of the following window
    int lastSource = -1, firstAfterOn = -1;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i].timeMs < 0)
            continue;
        if (firstAfterOn < 0)
            firstAfterOn = (int) i;
        if (state[i] == source)
            lastSource = (int) i;
    }
    int arriveIdx = -1;
    for (size_t i = (size_t) juce::jmax (firstAfterOn, lastSource + 1); i < frames.size() && arriveIdx < 0; ++i)
    {
        if (state[i] != target)
            continue;
        int voiced = 0, tgt = 0;
        for (size_t j = i; j < frames.size() && frames[j].timeMs - frames[i].timeMs <= p.arriveConfirmMs; ++j)
        {
            voiced += state[j] != none ? 1 : 0;
            tgt += state[j] == target ? 1 : 0;
        }
        if (voiced > 0 && tgt >= p.arriveConfirmShare * voiced)
            arriveIdx = (int) i;
    }
    if (arriveIdx < 0)
    {
        r.failure = "target pitch never established for good";
        return r;
    }
    // sub-frame: where the pitch crosses the band edge, if the neighbouring frame is a voiced glide
    // (an abrupt change has no in-between pitch: the frame time is the estimate)
    auto bandCrossing = [&] (int outside, int inside, const std::vector<double>& c, double band) {
        const double tIn = frames[(size_t) inside].timeMs;
        if (outside < 0 || outside >= (int) frames.size() || frames[(size_t) outside].hz <= 0)
            return tIn;
        const double e0 = std::abs (c[(size_t) outside]), e1 = std::abs (c[(size_t) inside]);
        if (e0 <= band || e1 > band || e0 > 4.0 * band || juce::approximatelyEqual (e0, e1))
            return tIn;
        const double f = juce::jlimit (0.0, 1.0, (e0 - band) / (e0 - e1));
        return frames[(size_t) outside].timeMs + f * (tIn - frames[(size_t) outside].timeMs);
    };
    r.arriveMs = juce::jmax (0.0, bandCrossing (arriveIdx - 1, arriveIdx, cT, bandT));
    if (lastSource >= 0)
        r.startMs = juce::jlimit (0.0, r.arriveMs, bandCrossing (lastSource + 1, lastSource, cS, bandS));
    else
        r.startMs = 0.0; // the source pitch was gone right at note-on (or never there)

    // --- glide or abrupt? Fit the pitch track with a ramp model: the source pitch, a linear glide (in
    // cents) from t1 to t2, the target pitch. Every frame reports roughly the average pitch over its
    // window (window + lag, ~2 periods of the lower note - 60 ms at 33 Hz), so the model is averaged over
    // each frame's window before comparing: that undoes the window's blur. If the best ramp explains the
    // track clearly better than the best instant change, it is a glide (portamento) and start / mid /
    // arrive come from the fitted ramp; otherwise the change is abrupt and is located sample-precisely
    // below. (Noise gaps hold no pitch frames, so they do not look like glides.)
    const double windowMs = pp.windowPeriods * 1000.0 / pp.minHz;
    r.midMs = 0.5 * (r.startMs + r.arriveMs);
    r.midFromPitch = false;
    {
        const double s0 = -interval + centreS - centreT; // nominal source level, cents re. the target centre
        // was the source steady at its pitch before the note-on? (in a fast portamento passage the previous
        // glide may still be under way: then the ramp's start level is unknown and only data after the
        // note-on is used)
        double cS0 = 0, madS0 = 0;
        const bool sourceSteady = steadyStats (fA, -1.0e9, -5.0, cS0, madS0) && madS0 < 20.0 && std::abs (cS0) < 50.0;
        struct Obs { double t, half, y; };
        std::vector<Obs> obs;
        // the band crossings sit inside the glide (the pitch is already moving within the bands), so the
        // range reaches well beyond them on both sides
        const double coarseSpan = std::abs (r.arriveMs - r.startMs);
        const double tLo = sourceSteady ? -20.0 : 0.0;
        const double tHi = juce::jmin (p.stableFromMs, juce::jmax (r.startMs, r.arriveMs) + juce::jmax (1.5 * windowMs, 2.0 * coarseSpan) + 20.0);
        for (size_t i = 0; i < frames.size(); ++i)
        {
            if (frames[i].timeMs < tLo || frames[i].timeMs > tHi || frames[i].hz <= 0 || frames[i].aperiodicity > 0.35 || cT[i] > 1.0e8)
                continue;
            const double y = cT[i]; // octave-corrected, re. the target centre
            if (y < juce::jmin (s0, 0.0) - 150.0 || y > juce::jmax (s0, 0.0) + 150.0)
                continue;
            if (! obs.empty() && frames[i].timeMs - obs.back().t < 0.99)
                continue; // 1 ms spacing is plenty (frames overlap heavily anyway)
            const double tauMs = 1000.0 / frames[i].hz;
            obs.push_back ({ frames[i].timeMs, 0.5 * (windowMs + tauMs), y });
        }
        // model: level s1 before t1, linear to 0 (the target) at t2. Each frame sees the path averaged over
        // its window: avg = s1 * Q, Q = 1 + (F(b) - F(a)) / (b - a) with F the integral of (q - 1), q = path / s1
        auto F = [] (double t, double t1, double t2) {
            if (t <= t1)
                return 0.0;
            if (t2 - t1 < 1.0e-9 || t >= t2)
                return -(0.5 * (t2 - t1) + (t - t2));
            const double d = t - t1;
            return -d * d / (2.0 * (t2 - t1));
        };
        auto Qof = [&] (const Obs& o, double t1, double t2) {
            const double a = o.t - o.half, b = o.t + o.half;
            return 1.0 + (F (b, t1, t2) - F (a, t1, t2)) / (b - a);
        };
        // best start level for a given ramp is linear least squares: s1 = sum(yQ) / sum(Q^2)
        auto fit = [&] (double t1, double t2, double& s1) {
            double yq = 0, qq = 0;
            for (auto& o : obs)
            {
                const double q = Qof (o, t1, t2);
                yq += o.y * q;
                qq += q * q;
            }
            s1 = qq > 1.0e-12 ? yq / qq : s0;
            if (s1 * s0 < 0.0 || std::abs (s1) > std::abs (s0) + 150.0)
                s1 = s0; // a start level on the wrong side of the target is not a legato from this source
            // truncated quadratic: a stray frame (octave slip, the other note's release) can't dominate
            double e = 0;
            for (auto& o : obs)
            {
                const double d = juce::jmin (60.0, std::abs (s1 * Qof (o, t1, t2) - o.y));
                e += d * d;
            }
            return e;
        };
        auto modelAt = [&] (const Obs& o, double t1, double t2, double s1) { return s1 * Qof (o, t1, t2); };
        if (obs.size() >= 20)
        {
            const double lo = juce::jmax (tLo, -20.0), hi = tHi;
            double sJ = s0, sR = s0, tmp = 0;
            // best instant change
            double bestJ = r.arriveMs, eJ = 1.0e300;
            for (double j = lo; j <= hi; j += 1.0)
                if (const double e = fit (j, j, tmp); e < eJ)
                    eJ = e, bestJ = j, sJ = tmp;
            for (double j = bestJ - 1.0; j <= bestJ + 1.0; j += 0.05)
                if (const double e = fit (j, j, tmp); e < eJ)
                    eJ = e, bestJ = j, sJ = tmp;
            // best ramp (coarse grid, then fine)
            double b1 = bestJ, b2 = bestJ, eR = eJ;
            sR = sJ;
            for (double t1 = lo; t1 <= hi; t1 += 2.0)
                for (double t2 = t1 + 2.0; t2 <= hi; t2 += 2.0)
                    if (const double e = fit (t1, t2, tmp); e < eR)
                        eR = e, b1 = t1, b2 = t2, sR = tmp;
            for (int pass = 0; pass < 2; ++pass)
            {
                const double span = pass == 0 ? 2.0 : 0.3, step = pass == 0 ? 0.25 : 0.05;
                const double c1 = b1, c2 = b2;
                for (double t1 = c1 - span; t1 <= c1 + span; t1 += step)
                    for (double t2 = juce::jmax (t1, c2 - span); t2 <= c2 + span; t2 += step)
                        if (const double e = fit (t1, t2, tmp); e < eR)
                            eR = e, b1 = t1, b2 = t2, sR = tmp;
            }
            const double rampMs = b2 - b1;
            // a glide must be long enough to be a glide and explain the track clearly better than a jump
            const bool glide = rampMs >= juce::jmax (8.0, 0.25 * windowMs) && eR < 0.5 * eJ;
            if (juce::SystemStats::getEnvironmentVariable ("LEGATO_DEBUG", "0") == "5")
            {
                std::printf ("    ramp fit: jump %.2f (err %.1f, s %.1f)  ramp %.2f..%.2f (err %.1f, s %.1f)  -> %s  s0 %.1f window %.1f ms steady %d\n",
                             bestJ, eJ, sJ, b1, b2, eR, sR, glide ? "glide" : "abrupt", s0, windowMs, sourceSteady ? 1 : 0);
                for (size_t k = 0; k < obs.size(); k += 5)
                    std::printf ("      t %7.2f  obs %8.2f  model %8.2f\n", obs[k].t, obs[k].y, modelAt (obs[k], b1, b2, sR));
            }
            if (glide)
            {
                const double absS = std::abs (sR);
                r.startMs = juce::jmax (0.0, b1 + rampMs * juce::jmin (1.0, bandS / absS));
                r.arriveMs = juce::jmax (0.0, absS > bandT ? b1 + rampMs * (1.0 - bandT / absS) : b1);
                r.midMs = 0.5 * (b1 + b2);
                r.midFromPitch = true;
            }
        }
    }

    // --- abrupt changes (slurs, re-bows): sample-precise boundaries from one-period residuals at the
    // two known periods. The forward residual at t compares x(u) with x(u + T_target) for u in
    // [t, t + period): ~0 exactly from the moment the target waveform starts. The backward residual
    // compares x(u) with x(u - T_source) for u in (t - period, t]: ~0 up to the moment the source
    // waveform ends. One period is much shorter than the pitch tracker's window (2 periods of the
    // lower note / 1.3), so this is what resolves low notes.
    if (! r.midFromPitch)
    {
        std::vector<float> x ((size_t) n, 0.0f);
        for (int c = 0; c < withB.getNumChannels(); ++c)
        {
            auto* src = withB.getReadPointer (c);
            for (int i = 0; i < n; ++i)
                x[(size_t) i] += src[i];
        }
        const double TB = fs / (fB * std::pow (2.0, centreT / 1200.0));
        const double TA = fs / (fA * std::pow (2.0, centreS / 1200.0));
        auto at = [&] (double pos) {
            if (pos < 0.0 || pos >= (double) (n - 1))
                return 0.0;
            const int i = (int) pos;
            const double fr = pos - i;
            return (double) x[(size_t) i] + fr * ((double) x[(size_t) i + 1] - (double) x[(size_t) i]);
        };
        auto resid = [&] (int t, double T, bool forward) {
            const int P = juce::jmax (8, (int) std::round (T));
            double num = 0, den = 1.0e-20;
            for (int k = 0; k < P; ++k)
            {
                const int u = forward ? t + k : t - k;
                const double a = at (u), b = at (u + (forward ? T : -T));
                num += (a - b) * (a - b);
                den += a * a + b * b;
            }
            return num / den;
        };
        const double zero = t0 + p.latencySamples;
        auto toSample = [&] (double v) { return (int) std::round (zero + v * fs / 1000.0); };
        auto toTime = [&] (double sample) { return (sample - zero) * 1000.0 / fs; };
        auto medianResid = [&] (double fromT, double toT, double T, bool forward) {
            std::vector<double> v;
            for (double tm = fromT; tm < toT; tm += 2.0)
            {
                const int t = toSample (tm);
                if (t - 2 * T >= 0 && t + 3 * T < n)
                    v.push_back (resid (t, T, forward));
            }
            return v.size() < 5 ? -1.0 : median (v);
        };
        const double mB = juce::jmax (0.0, medianResid (p.stableFromMs, p.stableToMs, TB, true));
        const double mAraw = medianResid (-55.0, -2.0, TA, false);
        const double mA = mAraw < 0 ? mB : mAraw;
        const bool bLower = fB < fA;
        // A point "is" a note when the waveform fits that note's period better than the other note's
        // (relative: a quieter release of the other note underneath, or a small interval whose periods
        // differ by only a few %, still leaves one clearly better), and fits it at all (cap: noise fits neither).
        // Octaves: the lower note's period also fits a higher-note waveform, so the higher note wins when it
        // fits, and the lower note only counts where the higher note's period clearly misfits.
        const double capB = juce::jlimit (0.35, 0.7, 3.0 * mB), capA = juce::jlimit (0.35, 0.7, 3.0 * mA);
        double dominance = p.octaveRejectDprime;
        if (octaves)
        {
            // the higher period's residual of each note alone; the boundary is in the middle
            const double Th = juce::jmin (TA, TB);
            const double srcAlone = medianResid (-55.0, -2.0, Th, false), tgtAlone = medianResid (p.stableFromMs, p.stableToMs, Th, true);
            if (srcAlone >= 0 && tgtAlone >= 0)
                dominance = 0.5 * (srcAlone + tgtAlone);
        }
        // the pitch track (robust over its ~2-period windows) vetoes points it clearly contradicts: noisy
        // material right after a bow change can make the wrong period look marginally better for a moment
        auto frameStateAt = [&] (int t) {
            const double tm = toTime (t);
            auto it = std::lower_bound (frames.begin(), frames.end(), tm, [] (const PitchFrame& f, double v) { return f.timeMs < v; });
            const size_t i = it == frames.end() ? frames.size() - 1 : (size_t) (it - frames.begin());
            return state[i];
        };
        const double margin = 0.8; // the winning period must fit clearly better
        // Octaves: in a mixture "higher note + lower note at relative amplitude a" the higher period's
        // residual is ~2a^2 / (1 + a^2) (only the lower note misfits there), so it measures which one
        // dominates: below `dominance` the higher note does (e.g. the source's release still sounding
        // an octave below the new note), above it the lower note does.
        // (Any mixture of the two notes repeats at the lower note's period - noise does not: for octaves the
        // lower period must fit, and the higher period's residual then says which note dominates.)
        auto isTarget = [&] (int t) {
            if (frameStateAt (t) == source)
                return false;
            const double rB = resid (t, TB, true);
            if (octaves)
                return bLower ? rB < capB && resid (t, TA, true) > dominance : rB < dominance && resid (t, TA, true) < capA;
            return rB < capB && rB < margin * resid (t, TA, true);
        };
        auto isSource = [&] (int t) {
            if (frameStateAt (t) == target)
                return false;
            const double rA = resid (t, TA, false);
            if (octaves)
                return bLower ? rA < dominance && resid (t, TB, false) < capB : rA < capA && resid (t, TB, false) > dominance;
            return rA < capA && rA < margin * resid (t, TB, false);
        };

        if (juce::SystemStats::getEnvironmentVariable ("LEGATO_DEBUG", "0") == "6")
        {
            std::printf ("    dominance %.3f (octaves %d) frameBoundary %.3f capA %.3f capB %.3f mA %.3f mB %.3f\n", dominance, octaves ? 1 : 0,
                         octaveFrameBoundary, capA, capB, mA, mB);
            for (double tm = 200.0; tm < 460.0; tm += 6.0)
            {
                const int t = toSample (tm);
                auto it = std::lower_bound (frames.begin(), frames.end(), tm, [] (const PitchFrame& f, double v) { return f.timeMs < v; });
                const auto& f = it == frames.end() ? frames.back() : *it;
                std::printf ("      %6.1f  pick %7.1f  dS %.2f dT %.2f st %d | fwdB %.3f fwdA %.3f bwdA %.3f bwdB %.3f  %s%s  lvl %.1f\n", tm, f.hz,
                             f.probe[0], f.probe[1], frameStateAt (t), resid (t, TB, true), resid (t, TA, true), resid (t, TA, false),
                             resid (t, TB, false), isTarget (t) ? "T" : "-", isSource (t) ? "S" : "-", f.rmsDb);
            }
        }
        if (juce::SystemStats::getEnvironmentVariable ("LEGATO_DEBUG", "0") == "3")
        {
            std::printf ("    TA %.2f TB %.2f mA %.4f mB %.4f capA %.3f capB %.3f centreS %.1f centreT %.1f\n", TA, TB, mA, mB, capA, capB, centreS, centreT);
            for (double tm = r.startMs - 40.0; tm < r.arriveMs + 40.0; tm += 2.0)
            {
                const int t = toSample (tm);
                if (t - 3 * TA < 0 || t + 3 * TB >= n)
                    continue;
                std::printf ("      %7.2f  fwdB %.3f fwdA %.3f  bwdA %.3f bwdB %.3f  %s%s\n", tm, resid (t, TB, true), resid (t, TA, true),
                             resid (t, TA, false), resid (t, TB, false), isTarget (t) ? "T" : "-", isSource (t) ? "S" : "-");
            }
        }
        // classify a fine grid over a generous range around the frame estimates
        const double fromMs = -40.0; // the source can only end after the note-on: scan from there (coarse estimates can be late)
        const double toMs2 = juce::jmin (p.stableFromMs, juce::jmax (r.arriveMs, r.startMs) + 2.0 * windowMs + 10.0);
        const int g0 = toSample (fromMs), g1 = toSample (toMs2);
        const int stride = juce::jmax (1, (int) std::round (fs * 0.00025));
        std::vector<int> gt, gs;
        for (int t = juce::jmax ((int) (3 * TA), g0); t <= g1 && t + 3 * TB < n; t += stride)
        {
            gt.push_back (isTarget (t) ? 1 : 0);
            gs.push_back (isSource (t) ? 1 : 0);
        }
        auto gridTime = [&] (size_t k) { return juce::jmax ((int) (3 * TA), g0) + (int) k * stride; };
        if (juce::SystemStats::getEnvironmentVariable ("LEGATO_DEBUG", "0") == "4")
        {
            juce::String line;
            for (size_t k = 0; k < gt.size(); k += 8)
                line << (gt[k] && gs[k] ? "B" : gt[k] ? "T" : gs[k] ? "S" : ".");
            std::printf ("    grid %.1f..%.1f ms (2 ms/char): %s\n", toTime (gridTime (0)), toTime (gridTime (gt.size() - 1)), line.toRawUTF8());
        }
        const int holdS = (int) std::ceil (juce::jmax (fs * 0.003, 1.0 * TA) / stride);
        const int K = (int) gt.size();
        // the source "for good": its last episode - a window of >= max(8 ms, 3 periods) at least half filled
        // with source points (in a crossfade the dominance flickers point to point, so runs alone miss a
        // real return of the source, while a blip of a few ms is not one)
        int sEnd = -1;
        {
            const int wS = (int) std::ceil (juce::jmax (fs * 0.008, 3.0 * TA) / stride);
            std::vector<int> cum ((size_t) K + 1, 0);
            for (int k = 0; k < K; ++k)
                cum[(size_t) k + 1] = cum[(size_t) k] + gs[(size_t) k];
            for (int k = K - 1; k >= 0 && sEnd < 0; --k)
            {
                const int a = juce::jmax (0, k - wS + 1);
                if (gs[(size_t) k] && (cum[(size_t) k + 1] - cum[(size_t) a]) * 2 >= wS && k - a + 1 >= juce::jmin (wS, holdS))
                    sEnd = k;
            }
        }
        // the target "for good": the first target point after the source's end where the target is the
        // clear majority of the classified points that follow (noise points in between don't break it)
        const int confirm = (int) std::ceil (juce::jmax (fs * 0.02, 4.0 * TB) / stride);
        auto confirmed = [&] (int k) {
            int t = 0, sN = 0;
            for (int j = k; j < juce::jmin (K, k + confirm); ++j)
            {
                t += gt[(size_t) j];
                sN += gs[(size_t) j];
            }
            return t >= juce::jmin (confirm / 4, 8) && t >= 0.6 * (t + sN);
        };
        // abrupt change with overlapping edges: the target is already there before the source has ended
        // (or within 1 ms after)
        const int lookBack = (int) std::ceil (2.0 * juce::jmax (TA, TB) / stride), after = (int) std::ceil (fs * 0.001 / stride);
        int tStart = -1;
        bool overlapping = false;
        if (sEnd >= 0)
            for (int k = juce::jmax (0, sEnd - lookBack); k <= juce::jmin (K - 1, sEnd + after) && tStart < 0; ++k)
                if (gt[(size_t) k] && confirmed (k))
                {
                    // walk back through the overlap (gaps up to 2 ms)
                    tStart = k;
                    const int maxGap = (int) std::ceil (fs * 0.002 / stride);
                    for (int j = k - 1, gap = 0; j >= 0; --j)
                    {
                        if (gt[(size_t) j])
                            tStart = j, gap = 0;
                        else if (++gap > maxGap)
                            break;
                    }
                    overlapping = true;
                }
        if (tStart < 0)
            for (int k = sEnd + 1; k < K; ++k)
                if (gt[(size_t) k] && confirmed (k))
                {
                    tStart = k;
                    break;
                }
        // A short stretch between the source's end and the target's start: noise (a bow change) or an abrupt
        // change whose two zones the margin kept apart? In noise neither period fits; across an abrupt change
        // of a small interval both fit fairly well.
        // Judged on per-sample residuals (each sample against its partner exactly one period away, smoothed
        // over 1 ms): the period-window residuals reach a period or two into the neighbouring notes.
        if (! overlapping && sEnd >= 0 && tStart > sEnd + 1)
        {
            const int g0s = gridTime ((size_t) sEnd + 1), g1s = gridTime ((size_t) tStart - 1);
            const int sm = juce::jmax (1, (int) std::round (fs * 0.0005));
            std::vector<double> m;
            for (int u = g0s; u <= g1s; u += juce::jmax (1, sm))
            {
                double na = 0, nb = 0, den = 1.0e-20;
                for (int k = u - sm; k <= u + sm; ++k)
                {
                    const double xk = at (k), a = xk - at (k - TA), b = xk - at (k + TB);
                    na += a * a;
                    nb += b * b;
                    den += xk * xk;
                }
                m.push_back (juce::jmin (na, nb) / den);
            }
            overlapping = m.empty() || median (m) < 0.5; // noise: neither period fits (~1-2)
        }
        if (juce::SystemStats::getEnvironmentVariable ("LEGATO_DEBUG", "0") == "4")
            std::printf ("    sEnd %.2f ms  tStart %.2f ms  overlapping %d\n", sEnd >= 0 ? toTime (gridTime ((size_t) sEnd)) : -1.0,
                         tStart >= 0 ? toTime (gridTime ((size_t) tStart)) : -1.0, overlapping ? 1 : 0);
        if (tStart >= 0)
        {
            // sample-precise edges
            int ta = gridTime ((size_t) tStart);
            while (ta - 1 > g0 && isTarget (ta - 1))
                --ta;
            double arrive = ta, startS = -1;
            if (sEnd >= 0)
            {
                int ts = gridTime ((size_t) sEnd);
                while (ts + 1 < g1 && isSource (ts + 1))
                    ++ts;
                startS = ts;
            }
            if (overlapping && startS >= 0)
            {
                // The two edges overlap: an abrupt change, and for a while the waveform fits both periods
                // (a small interval on a low note changes phase only slowly). Least squares on the change
                // point J: source-period misfit before J + target-period misfit after J (per-sample residuals
                // normalised by the local power) is minimal at the change itself.
                double J = 0.5 * (arrive + startS);
                {
                    const double Tmax = juce::jmax (TA, TB);
                    const int L = juce::jmax ((int) std::ceil (Tmax) + 1, (int) std::floor (juce::jmin (arrive, startS) - Tmax));
                    const int U = juce::jmin (n - (int) std::ceil (Tmax) - 2, (int) std::ceil (juce::jmax (arrive, startS) + Tmax));
                    if (U - L > 8)
                    {
                        const int half = juce::jmax (4, (int) std::round (0.5 * Tmax));
                        const int m = U - L + 1;
                        std::vector<double> pw ((size_t) m), ra ((size_t) m), rb ((size_t) m);
                        for (int u = L; u <= U; ++u)
                        {
                            double e = 0;
                            for (int k = -half; k <= half; ++k)
                                e += at (u + k) * at (u + k);
                            pw[(size_t) (u - L)] = e / (2 * half + 1) + 1.0e-12;
                        }
                        // Local power over exactly one period of each comparison's lag (a whole period of a
                        // periodic signal has constant power): the gain between a sample and its partner one
                        // period away compensates amplitude changes (bow-change dips, crossfades), so only the
                        // waveform shape is compared
                        std::vector<double> sq ((size_t) n + 1, 0.0);
                        for (int i = 0; i < n; ++i)
                            sq[(size_t) i + 1] = sq[(size_t) i] + (double) x[(size_t) i] * x[(size_t) i];
                        auto powerAt = [&] (double c, double T) {
                            const int len = juce::jmax (4, (int) std::round (T));
                            const int a0 = juce::jlimit (0, n - len, (int) std::round (c) - len / 2);
                            return (sq[(size_t) (a0 + len)] - sq[(size_t) a0]) / len + 1.0e-12;
                        };
                        // only where the level really moves (> 1.5 dB within a period): otherwise the gains are 1,
                        // because near the change a one-period window of one note is not a whole number of the
                        // other note's periods, and its ripple would bias J
                        bool levelMoves = false;
                        for (int u = L; u <= U && ! levelMoves; u += 8)
                            levelMoves = std::abs (10.0 * std::log10 (powerAt (u, 2.0 * Tmax) / powerAt (u - Tmax, 2.0 * Tmax))) > 1.5;
                        for (int u = L; u <= U; ++u)
                        {
                            const double pa = powerAt (u, TA), pb = powerAt (u, TB);
                            const double ga = levelMoves ? std::sqrt (pa / powerAt (u - TA, TA)) : 1.0;
                            const double gb = levelMoves ? std::sqrt (pb / powerAt (u + TB, TB)) : 1.0;
                            const double xu = at (u), da = xu - ga * at (u - TA), db = xu - gb * at (u + TB);
                            ra[(size_t) (u - L)] = da * da / pa;
                            rb[(size_t) (u - L)] = db * db / pb;
                        }
                        if (octaves)
                        {
                            // the lower period fits a higher-note waveform too: before J the higher note must
                            // NOT fit (if it is the target), after J the higher source must not fit (if the
                            // source is higher). Penalty = how well the higher period fits, over half a period
                            const double Th = juce::jmin (TA, TB);
                            const int w = juce::jmax (4, (int) std::round (0.5 * Th));
                            auto fitHigh = [&] (int u, bool backward) {
                                double num = 0, den = 1.0e-20;
                                for (int k = u - w; k <= u + w; ++k)
                                {
                                    const double a = at (k), b = at (k + (backward ? -Th : Th));
                                    num += (a - b) * (a - b);
                                    den += a * a + b * b;
                                }
                                return juce::jmax (0.0, 1.0 - num / den); // 1 = the higher period fits perfectly
                            };
                            for (int u = L; u <= U; ++u)
                            {
                                if (! bLower)
                                    ra[(size_t) (u - L)] += 4.0 * fitHigh (u, true);  // target higher: penalise it before J
                                else
                                    rb[(size_t) (u - L)] += 4.0 * fitHigh (u, false); // source higher: penalise it after J
                            }
                        }
                        // E(j) = sum_{u < j} ra + sum_{u >= j} rb
                        double eCur = 0;
                        for (auto v : rb)
                            eCur += v;
                        double best = eCur;
                        int bestJ = L;
                        std::vector<double> E ((size_t) m + 1);
                        E[0] = eCur;
                        for (int j = L + 1; j <= U + 1; ++j)
                        {
                            eCur += ra[(size_t) (j - 1 - L)] - rb[(size_t) (j - 1 - L)];
                            E[(size_t) (j - L)] = eCur;
                            if (eCur < best)
                            {
                                best = eCur;
                                bestJ = j;
                            }
                        }
                        J = bestJ;
                        const int bi = bestJ - L;
                        if (bi > 0 && bi + 1 < (int) E.size())
                        {
                            const double y0 = E[(size_t) bi - 1], y1 = E[(size_t) bi], y2 = E[(size_t) bi + 1];
                            const double den = y0 - 2.0 * y1 + y2;
                            if (den > 1.0e-12)
                                J += juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den);
                        }
                    }
                }
                arrive = startS = J;
            }
            else
            {
                // separate edges (noise or a bow change between): each edge by least squares against a
                // constant "not this note" cost - per-sample normalised residuals are ~0 for the note and ~2
                // for unrelated sound, so 1 sits in the middle
                auto edge = [&] (double guess, double T, bool target) {
                    const int L = juce::jmax ((int) std::ceil (T) + 1, (int) std::floor (guess - 2.0 * T));
                    const int U = juce::jmin (n - (int) std::ceil (T) - 2, (int) std::ceil (guess + 2.0 * T));
                    if (U - L < 8)
                        return guess;
                    const int half = juce::jmax (4, (int) std::round (0.5 * T));
                    std::vector<double> rr ((size_t) (U - L + 1));
                    for (int u = L; u <= U; ++u)
                    {
                        double e = 0;
                        for (int k = -half; k <= half; ++k)
                            e += at (u + k) * at (u + k);
                        const double d = at (u) - at (u + (target ? T : -T));
                        rr[(size_t) (u - L)] = juce::jmin (4.0, d * d / (e / (2 * half + 1) + 1.0e-12));
                    }
                    // the constant cost sits between the residual levels on the two sides of the edge
                    auto medianOf = [&] (int lo, int hi) {
                        std::vector<double> v;
                        for (int u = juce::jmax (L, lo); u <= juce::jmin (U, hi); ++u)
                            v.push_back (rr[(size_t) (u - L)]);
                        return median (v);
                    };
                    const int g = (int) std::round (guess);
                    const double inside = target ? medianOf (g + (int) T, U) : medianOf (L, g - (int) T);
                    const double outside = target ? medianOf (L, g - (int) T) : medianOf (g + (int) T, U);
                    if (! (outside > 1.5 * inside + 0.05))
                        return guess; // no clear contrast: keep the grid edge
                    const double c = 0.5 * (inside + outside);
                    // target: cost c before J, residual after; source: residual before J, cost c after
                    double eCur = 0;
                    for (auto v : rr)
                        eCur += target ? v : c;
                    double best = eCur;
                    int bestJ = L;
                    for (int j = L + 1; j <= U + 1; ++j)
                    {
                        const double v = rr[(size_t) (j - 1 - L)];
                        eCur += target ? (c - v) : (v - c);
                        if (eCur < best)
                        {
                            best = eCur;
                            bestJ = j;
                        }
                    }
                    return (double) bestJ;
                };
                arrive = edge (arrive, TB, true);
                if (startS >= 0)
                    startS = juce::jmin (arrive, edge (startS, TA, false));
            }
            r.arriveMs = juce::jmax (0.0, toTime (arrive));
            if (startS >= 0)
                r.startMs = juce::jlimit (0.0, r.arriveMs, toTime (startS));
        }
        r.startMs = juce::jmin (r.startMs, r.arriveMs);
        r.midMs = 0.5 * (r.startMs + r.arriveMs);
    }

    int voiced = 0, total = 0;
    for (size_t i = (size_t) firstAfterOn; i < frames.size() && frames[i].timeMs <= r.arriveMs + 100.0; ++i, ++total)
        voiced += state[i] != none ? 1 : 0;
    r.voicedFraction = total > 0 ? (double) voiced / total : 0.0;
    r.valid = true;
    return r;
}

} // namespace tp
