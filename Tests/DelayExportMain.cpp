// Writes the compact delays file for saved results: DelayExport <results.json>... (-> <name>.delays.json)
#include "../Source/DelayExport.h"
#include <iostream>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: DelayExport <results.json>...\n";
        return 2;
    }
    int failed = 0;
    for (int i = 1; i < argc; ++i)
    {
        const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[i]));
        if (f.getFileName().endsWith (".delays.json"))
            continue;
        const auto r = tp::writeCompactDelays (f);
        std::cout << (r.wasOk() ? "wrote " + tp::delaysFileFor (f).getFullPathName() : r.getErrorMessage()) << "\n";
        failed += r.failed() ? 1 : 0;
    }
    return failed > 0 ? 1 : 0;
}
