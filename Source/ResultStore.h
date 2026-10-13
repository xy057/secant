#pragma once

#include "RunTypes.h"

namespace tp
{
// Thread-safe sink for hits from all render workers, plus the JSON exporter.
class ResultStore
{
public:
    void reset (std::vector<ArticulationPlan> plans, const RunSettings&, const Instrument&, const Collection&);

    // one finished job = one articulation x one note: all its hits and per-layer RR series
    void addJob (int planIdx, std::vector<Hit>&& hits, std::vector<Series>&& series);
    void addLegatoJob (int planIdx, std::vector<LegatoHit>&& transitions);
    void log (const juce::String& line);
    juce::StringArray drainLog();

    // progress is counted in jobs (the number of hits is only known once each RR cycle closes)
    int64_t totalJobs() const { return total; }
    int64_t doneJobs() const { return done.load(); }
    int64_t hitsRendered() const { return hitCount.load(); }
    int64_t doneJobsFor (int planIdx) const;
    int64_t totalJobsFor (int planIdx) const;
    juce::String rrSummaryFor (int planIdx) const;  // e.g. "5" or "3-5" (detected RR cycle length)
    double medianPrimaryFor (int planIdx) const;    // NaN if none yet
    double medianLegatoFor (int planIdx) const;     // legato primary over all valid transitions, NaN if none yet
    int64_t legatoTransitionsFor (int planIdx) const;

    struct PluginInfo
    {
        juce::String name, manufacturer, version, fileOrIdentifier;
        int latencySamples = 0;
        int numOutputChannels = 0;
        juce::String stateFile; // saved plugin state used for this run
    };
    void setPluginInfo (const PluginInfo& p) { plugin = p; }
    void addWarning (const juce::String& w);
    void setResolvedDynamics (int planIdx, DynamicsSource); // result of a worker's auto-detection
    // the player's own configuration (SINE state), exported for reference
    void setSineInfo (const SineState&, const juce::StringArray& notes);
    // the determinism check's result; forcedNever: it switched the run to RR reset Never
    void setDeterminism (const juce::var& result, bool forcedNever);

    juce::var toJson() const;
    juce::Result writeJson (const juce::File&) const;

private:
    int64_t totalJobsForLocked (size_t planIdx) const;
    juce::var legatoJson (size_t planIdx, juce::StringArray& warningsOut) const;
    juce::var sineJson() const;

    mutable juce::CriticalSection lock;
    std::vector<ArticulationPlan> plans;
    std::vector<std::vector<Hit>> hits;
    std::vector<std::vector<Series>> series;
    std::vector<std::vector<LegatoHit>> legato;
    SineState sine;
    juce::StringArray sineNotes;
    juce::var determinism;
    std::vector<int64_t> jobsDonePer;
    RunSettings settings;
    juce::String instrumentName, instrumentGroup, collectionName, collectionSource, vendor;
    PluginInfo plugin;
    juce::StringArray warnings, pendingLog;
    juce::Time startedAt;
    int64_t total = 0;
    std::atomic<int64_t> done { 0 }, hitCount { 0 };
};

} // namespace tp
