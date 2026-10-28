// Renders the plug-in editor with a demo pattern to a PNG (for docs and visual checks).
#include "../plugin/PluginEditor.h"
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    DrumKitProcessor p;
    p.prepareToPlay (48000, 512);
    using C = DrumKitProcessor::Cmd;
    for (int s = 0; s < DK_STEPS; s += 8) p.send (C::ToggleStep, DK_KICK, s);
    for (int s = 4; s < DK_STEPS; s += 8) p.send (C::ToggleStep, DK_SNARE, s);
    for (int s = 0; s < DK_STEPS; s += 2) p.send (C::ToggleStep, DK_CHH, s);
    p.send (C::ToggleStep, DK_KICK, 14); p.send (C::ToggleStep, DK_CLAP, 28); p.send (C::ToggleStep, DK_CRASH, 0);
    p.send (C::TogglePlay);
    juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m;
    for (int i = 0; i < 40; ++i) p.processBlock (b, m);
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    juce::File out (argc > 1 ? argv[1] : "plugin.png");
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat().writeImageToStream (img, os);
    return 0;
}
