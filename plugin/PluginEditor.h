#pragma once
#include "PluginProcessor.h"

// One custom-drawn panel: transport, ten pads and the 32-step sequencer, same layout as the web version.
class DrumKitEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DrumKitEditor (DrumKitProcessor&);
    ~DrumKitEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum Ctl { Play, Rec, Clear, BpmDown, BpmUp, Click, Quant, NumCtl };
    DrumKitProcessor& proc;
    juce::Rectangle<int> ctl[NumCtl], bpmBox, loopBar, pad[DK_NI], seqArea, gridArea, meter;
    std::array<uint32_t, DK_NI> seenHits {};
    std::array<double, DK_NI> flashUntil {};
    double clearDown = -1.0;
    bool clearDone = false;
    juce::Slider volume;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttach;

    void timerCallback() override;
    juce::Rectangle<int> cellBounds (int row, int step) const;
    void hitPad (int inst, int vel);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumKitEditor)
};
