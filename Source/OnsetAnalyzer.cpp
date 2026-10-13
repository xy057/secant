#include "OnsetAnalyzer.h"
#include <juce_dsp/juce_dsp.h>
#include <map>

namespace tp
{
namespace
{
    double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
    double gainToDb (double g) { return g > 0 ? 20.0 * std::log10 (g) : -200.0; }

    // Channel-summed energy per sample: e[n] = sum_c x_c[n]^2
    std::vector<double> energy (const juce::AudioBuffer<float>& b)
    {
        std::vector<double> e ((size_t) b.getNumSamples(), 0.0);
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* x = b.getReadPointer (c);
            for (int n = 0; n < b.getNumSamples(); ++n)
                e[(size_t) n] += (double) x[n] * x[n];
        }
        return e;
    }

    // Channel-summed squared magnitude of the analytic signal, |x + jH(x)|^2.
    // Zero-padded to >= 2N so the Hilbert transform's non-causal tail doesn't wrap around.
    std::vector<double> analyticEnergy (const juce::AudioBuffer<float>& b)
    {
        const int N = b.getNumSamples();
        int order = 1;
        while ((1 << order) < 2 * N)
            ++order;
        const int size = 1 << order;
        // vDSP FFT setups are expensive to create: keep one per order per thread
        thread_local std::map<int, std::unique_ptr<juce::dsp::FFT>> ffts;
        auto& fftPtr = ffts[order];
        if (fftPtr == nullptr)
            fftPtr = std::make_unique<juce::dsp::FFT> (order);
        auto& fft = *fftPtr;
        std::vector<juce::dsp::Complex<float>> buf ((size_t) size), spec ((size_t) size);
        std::vector<double> e ((size_t) N, 0.0);

        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* x = b.getReadPointer (c);
            for (int n = 0; n < size; ++n)
                buf[(size_t) n] = { n < N ? x[n] : 0.0f, 0.0f };
            fft.perform (buf.data(), spec.data(), false);
            for (int k = 1; k < size / 2; ++k)
                spec[(size_t) k] *= 2.0f;
            for (int k = size / 2 + 1; k < size; ++k)
                spec[(size_t) k] = {};
            fft.perform (spec.data(), buf.data(), true); // JUCE normalises the inverse by 1/size
            for (int n = 0; n < N; ++n)
                e[(size_t) n] += (double) std::norm (buf[(size_t) n]);
        }
        return e;
    }

    // Zero-phase (centred) moving RMS via prefix sums, normalised per channel
    // so a full-scale sine on every channel reads ~ -3 dBFS regardless of channel count.
    std::vector<double> centredRms (const std::vector<double>& e, int halfWidth, int numChannels)
    {
        const auto N = e.size();
        std::vector<double> prefix (N + 1, 0.0);
        for (size_t i = 0; i < N; ++i)
            prefix[i + 1] = prefix[i] + e[i];

        std::vector<double> rms (N, 0.0);
        const double chans = juce::jmax (1, numChannels);
        for (size_t i = 0; i < N; ++i)
        {
            auto lo = (size_t) juce::jmax<long long> (0, (long long) i - halfWidth);
            auto hi = juce::jmin (N, i + (size_t) halfWidth + 1);
            rms[i] = std::sqrt ((prefix[hi] - prefix[lo]) / (double) (hi - lo) / chans);
        }
        return rms;
    }

    // First index >= from where v[i] >= thr, refined by linear interpolation
    // between i-1 and i. Returns -1 if never crossed before `to`.
    double firstCrossing (const std::vector<double>& v, double thr, int from, int to)
    {
        for (int i = juce::jmax (from, 1); i < to; ++i)
        {
            if (v[(size_t) i] >= thr)
            {
                const double a = v[(size_t) i - 1], b = v[(size_t) i];
                if (a >= thr || b <= a)
                    return (double) i;
                return (double) (i - 1) + (thr - a) / (b - a);
            }
        }
        return -1.0;
    }

    // Parabolic peak refinement around index i
    double parabolic (const std::vector<double>& v, int i)
    {
        if (i <= 0 || i >= (int) v.size() - 1)
            return (double) i;
        const double a = v[(size_t) i - 1], b = v[(size_t) i], c = v[(size_t) i + 1];
        const double d = a - 2 * b + c;
        if (std::abs (d) < 1e-30)
            return (double) i;
        return (double) i + 0.5 * (a - c) / d;
    }
} // namespace

juce::StringArray onsetMethodKeys()
{
    juce::StringArray k { "first" };
    for (auto t : kRelThresholdsDb)
        k.add ("rel" + juce::String ((int) t));
    k.add ("maxSlope");
    k.add ("attackEnd");
    k.add ("attackPeak");
    k.add ("peak");
    return k;
}

double OnsetResult::get (const juce::String& method) const
{
    if (method == "first") return firstMs;
    if (method == "maxSlope") return maxSlopeMs;
    if (method == "peak") return peakMs;
    if (method == "attackEnd") return attackEndMs;
    if (method == "attackPeak") return attackPeakMs;
    for (size_t i = 0; i < std::size (kRelThresholdsDb); ++i)
        if (method == "rel" + juce::String ((int) kRelThresholdsDb[i]))
            return relMs[i];
    return firstMs;
}

OnsetResult analyseOnset (const juce::AudioBuffer<float>& capture, const OnsetParams& p)
{
    OnsetResult r;
    const int N = capture.getNumSamples();
    const int t0 = p.preSamples;
    if (N <= t0 + 4)
        return r;

    const double fs = p.sampleRate;
    auto toMs = [&] (double idx) { return (idx - t0 - p.latencySamples) * 1000.0 / fs; };

    // --- bleed check: anything audible before note-on?
    float pre = 0;
    for (int c = 0; c < capture.getNumChannels(); ++c)
        pre = juce::jmax (pre, capture.getMagnitude (c, 0, t0));
    r.preRollDb = gainToDb (pre);

    // --- "first": per-sample peak magnitude across channels
    std::vector<double> mag ((size_t) N, 0.0);
    for (int c = 0; c < capture.getNumChannels(); ++c)
    {
        auto* x = capture.getReadPointer (c);
        for (int n = 0; n < N; ++n)
            mag[(size_t) n] = juce::jmax (mag[(size_t) n], (double) std::abs (x[n]));
    }
    const int searchEnd = juce::jmin (N, t0 + p.latencySamples + (int) std::ceil (p.peakWindowMs * fs / 1000.0));
    // "first sound" = first sample above max(absThresholdDb, 10 dB over the previous note's tail)
    const double first = firstCrossing (mag, juce::jmax (dbToGain (p.absThresholdDb), 3.16 * (double) pre), t0, searchEnd);
    if (first < 0)
        return r; // silent

    // --- envelopes
    const bool hilbert = p.envelope == EnvelopeMode::hilbert;
    const auto e = hilbert ? analyticEnergy (capture) : energy (capture);
    // analytic magnitude is already an amplitude; rms of a sine is 1/sqrt(2) of it - rescale so both read as amplitude
    const double modeScale = hilbert ? 1.0 : std::sqrt (2.0);
    const double periodMs = p.fundamentalHz > 0 ? p.periodSmoothing * 1000.0 / p.fundamentalHz : 0.0;
    const double envMs = juce::jmax (hilbert ? p.hilbertSmoothMs : p.rmsWindowMs, periodMs);
    const double slopeMs = juce::jmax (p.slopeWindowMs, periodMs);
    auto env = centredRms (e, juce::jmax (1, (int) std::round (envMs * fs / 2000.0)), capture.getNumChannels());
    auto envSlow = centredRms (e, juce::jmax (1, (int) std::round (slopeMs * fs / 2000.0)), capture.getNumChannels());
    for (auto* v : { &env, &envSlow })
        for (auto& x : *v)
            x *= modeScale;

    // reference peak inside the window
    int peakIdx = t0;
    for (int i = t0; i < searchEnd; ++i)
        if (env[(size_t) i] > env[(size_t) peakIdx])
            peakIdx = i;
    const double peak = env[(size_t) peakIdx];
    if (peak <= 0)
        return r;

    r.valid = true;
    r.firstMs = toMs (first);
    r.peakMs = toMs (parabolic (env, peakIdx));
    r.peakDb = gainToDb (peak);

    // Relative threshold crossings, searched from the first non-silent sample up to the peak.
    // A sampler outputs digital silence before the voice starts, so nothing can legitimately
    // cross before `first`; this also discards the Hilbert transform's non-causal pre-ringing.
    const int searchFrom = juce::jmax (t0, (int) std::floor (first));
    for (size_t k = 0; k < std::size (kRelThresholdsDb); ++k)
    {
        const double thr = peak * dbToGain (kRelThresholdsDb[k]);
        const double x = firstCrossing (env, thr, searchFrom, peakIdx + 1);
        // already above threshold where sound begins: the crossing lies inside the smoothing blur,
        // so the first audible sample is the best estimate
        const bool atStart = x >= 0 && x <= searchFrom + 1 && env[(size_t) searchFrom] >= thr;
        r.relMs[k] = toMs (atStart ? first : (x >= 0 ? x : (double) peakIdx));
    }

    // steepest rise of the smoother envelope between first sound and the peak
    {
        const int lo = juce::jmax (t0 + 1, searchFrom);
        const int hi = juce::jmax (lo + 1, juce::jmin (N - 1, peakIdx + 1));
        std::vector<double> d ((size_t) N, 0.0);
        int best = lo;
        for (int i = lo; i < hi; ++i)
        {
            d[(size_t) i] = envSlow[(size_t) i + 1] - envSlow[(size_t) i - 1];
            if (d[(size_t) i] > d[(size_t) best])
                best = i;
        }
        r.maxSlopeMs = toMs (parabolic (d, best));
    }

    // end of attack / start of decay
    {
        const double attackMs = juce::jmax (p.attackSmoothMs, p.fundamentalHz > 0 ? p.attackPeriods * 1000.0 / p.fundamentalHz : 0.0);
        auto envA = centredRms (e, juce::jmax (1, (int) std::round (attackMs * fs / 2000.0)), capture.getNumChannels());
        for (auto& x : envA)
            x *= modeScale;

        // start where the attack is clearly under way: the rel-20 crossing (index 2 of kRelThresholdsDb)
        const int start = juce::jlimit (searchFrom, N - 1, (int) std::floor (r.relMs[2] * fs / 1000.0 + t0 + p.latencySamples));
        const int hold = juce::jmax (1, (int) std::round (p.attackPlateauMs * fs / 1000.0));
        const double decay = dbToGain (-p.attackDecayDb), plateauRise = dbToGain (p.attackPlateauRiseDb);
        const int walkEnd = juce::jmin (N, searchEnd + hold);

        double M = envA[(size_t) start];
        int m = start;
        for (int k = start + 1; k < walkEnd; ++k)
        {
            const double v = envA[(size_t) k];
            if (v > M)
            {
                M = v;
                m = k;
            }
            if (v < M * decay)
                break; // decay has started: the maximum before it ends the attack
            if (k - start >= hold && v < envA[(size_t) (k - hold)] * plateauRise)
            {
                // settled into a sustain: the plateau began at k - hold; anything after is slow swell
                m = start;
                for (int j = start; j <= k - hold; ++j)
                    if (envA[(size_t) j] > envA[(size_t) m])
                        m = j;
                M = envA[(size_t) m];
                break;
            }
        }
        r.attackPeakMs = toMs (parabolic (envA, m));
        r.attackPeakDb = gainToDb (M);
        const double x = firstCrossing (envA, M * dbToGain (p.attackEndDb), juce::jmax (searchFrom, start - hold), m + 1);
        r.attackEndMs = toMs (x >= 0 ? x : (double) m);
    }
    return r;
}

double similarity (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int start, int numSamples)
{
    const int chans = juce::jmin (a.getNumChannels(), b.getNumChannels());
    const int end = juce::jmin (a.getNumSamples(), b.getNumSamples(), start + numSamples);
    double ab = 0, aa = 0, bb = 0;
    for (int c = 0; c < chans; ++c)
    {
        auto* x = a.getReadPointer (c);
        auto* y = b.getReadPointer (c);
        for (int n = start; n < end; ++n)
        {
            ab += (double) x[n] * y[n];
            aa += (double) x[n] * x[n];
            bb += (double) y[n] * y[n];
        }
    }
    if (aa <= 0 || bb <= 0)
        return 0;
    return ab / std::sqrt (aa * bb);
}

double similarityLagged (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int start, int numSamples,
                         int maxLag, int& bestLag)
{
    const int chans = juce::jmin (a.getNumChannels(), b.getNumChannels());
    const int lo = juce::jmax (start, maxLag);
    const int hi = juce::jmin (a.getNumSamples(), b.getNumSamples() - maxLag, start + numSamples);
    double best = -2.0;
    bestLag = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double ab = 0, aa = 0, bb = 0;
        for (int c = 0; c < chans; ++c)
        {
            auto* x = a.getReadPointer (c);
            auto* y = b.getReadPointer (c);
            for (int n = lo; n < hi; ++n)
            {
                const double u = x[n], v = y[n + lag];
                ab += u * v;
                aa += u * u;
                bb += v * v;
            }
        }
        const double sim = (aa > 0 && bb > 0) ? ab / std::sqrt (aa * bb) : 0.0;
        if (sim > best)
        {
            best = sim;
            bestLag = lag;
        }
    }
    return best;
}

} // namespace tp
