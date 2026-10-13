// Inspects a SINE Player state (.pluginstate written by Secant):
//   SineStateTool <state>                 -> container layout + every articulation's RR reset time
//   SineStateTool <state> --json          -> the raw JSON
//   SineStateTool <state> --rr-reset <ms> -> patches the reset time (all articulations), re-parses, prints
#include <juce_core/juce_core.h>
#include <iostream>
#include "../Source/SineState.h"

using namespace tp;

int main (int argc, char** argv)
{
    if (argc < 2)
        return 2;
    juce::MemoryBlock st;
    juce::File (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])).loadFileAsData (st);
    const juce::String mode = argc > 2 ? argv[2] : "";
    if (mode == "--json")
    {
        std::cout << extractSineJson (st) << "\n";
        return 0;
    }
    if (mode == "--rr-reset" && argc > 3)
    {
        juce::String err;
        auto patched = withRoundRobinReset (st, juce::String (argv[3]).getDoubleValue(), &err);
        if (patched.isEmpty())
        {
            std::cout << "patch failed: " << err << "\n";
            return 1;
        }
        std::cout << "patched state: " << patched.getSize() << " bytes (was " << st.getSize() << ")\n";
        st = patched;
    }
    auto s = parseSineState (st);
    std::cout << "valid " << s.valid << " " << s.error << "\n";
    if (auto* inst = s.instrument())
        for (auto& a : inst->articulations)
            std::cout << "  " << a.title << ": rr " << a.roundRobins << ", reset " << a.rrResetMs << " ms\n";
    return 0;
}
