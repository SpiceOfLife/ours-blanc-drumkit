#include "PluginEditor.h"

namespace
{
    // screen order of the pads (5 × 2) and the computer key for each instrument
    constexpr int order[DK_NI] = { DK_KICK, DK_SNARE, DK_CLAP, DK_CHH, DK_OHH, DK_TOM_HI, DK_TOM_MID, DK_TOM_LO, DK_CRASH, DK_RIDE };
    constexpr const char* shortName[DK_NI] = { "KICK", "SNARE", "CL HAT", "OP HAT", "TOM 1", "TOM 2", "TOM 3", "CLAP", "CRASH", "RIDE" };
    constexpr char keyFor[DK_NI] = { 'A', 'S', 'F', 'G', 'H', 'J', 'K', 'D', 'L', ';' };

    const juce::Colour desk { 0xffE8EAE5 }, casing { 0xffD9DDD6 }, well { 0xffC9CEC6 }, edge { 0xffB4BAB1 },
                       ink { 0xff1D2022 }, muted { 0xff586058 }, rec { 0xffE0412F }, seqBg { 0xff23272A },
                       cellOff { 0xff31363A }, cellBeat { 0xff3A4044 }, lcd { 0xffFFD25A };

    juce::Colour instColour (int i) { return juce::Colour ((juce::uint32) (0xff000000u | dk_inst_color[i])); }
    juce::Font font (float h, bool bold = true) { return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain)); }
}

DrumKitEditor::DrumKitEditor (DrumKitProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setColour (juce::Slider::thumbColourId, ink);
    volume.setColour (juce::Slider::trackColourId, ink);
    volume.setColour (juce::Slider::backgroundColourId, edge);
    addAndMakeVisible (volume);
    volumeAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "volume", volume);

    setWantsKeyboardFocus (true);
    setSize (960, 640);
    startTimerHz (40);
}

DrumKitEditor::~DrumKitEditor() { stopTimer(); }

void DrumKitEditor::resized()
{
    auto r = getLocalBounds().reduced (24);
    r.removeFromTop (64);                                   // name plate
    auto bar = r.removeFromTop (48);
    int x = bar.getX();
    auto place = [&] (int w) { auto b = juce::Rectangle<int> (x, bar.getY(), w, bar.getHeight()); x += w + 10; return b; };
    ctl[Play] = place (96); ctl[Rec] = place (96); ctl[Clear] = place (130);
    bpmBox = place (176);
    ctl[BpmDown] = bpmBox.withWidth (40).reduced (4);
    ctl[BpmUp] = bpmBox.withX (bpmBox.getX() + 90).withWidth (40).reduced (4);
    ctl[Click] = place (96); ctl[Quant] = place (120);
    volume.setBounds (bar.getRight() - 120, bar.getY() + 8, 120, 32);

    r.removeFromTop (12);
    loopBar = r.removeFromTop (6);
    r.removeFromTop (12);

    auto padsArea = r.removeFromTop (196);
    meter = padsArea.removeFromRight (12);
    padsArea.removeFromRight (12);
    const int gap = 12, pw = (padsArea.getWidth() - gap * 4) / 5, ph = (padsArea.getHeight() - gap) / 2;
    for (int k = 0; k < DK_NI; ++k)
        pad[order[k]] = { padsArea.getX() + (k % 5) * (pw + gap), padsArea.getY() + (k / 5) * (ph + gap), pw, ph };

    r.removeFromTop (18);
    seqArea = r;
    gridArea = seqArea.reduced (14).withTrimmedTop (30).withTrimmedLeft (70);
}

juce::Rectangle<int> DrumKitEditor::cellBounds (int row, int step) const
{
    const float cw = (gridArea.getWidth() - 6.0f) / DK_STEPS, ch = gridArea.getHeight() / (float) DK_NI;
    const float x = gridArea.getX() + step * cw + (step >= 16 ? 6.0f : 0.0f);
    return juce::Rectangle<float> (x, gridArea.getY() + row * ch, cw - 3.0f, ch - 3.0f).toNearestInt();
}

void DrumKitEditor::paint (juce::Graphics& g)
{
    const auto& s = proc.snapshot();
    const double now = juce::Time::getMillisecondCounterHiRes();
    g.fillAll (desk);
    g.setColour (casing);
    g.fillRoundedRectangle (getLocalBounds().reduced (8).toFloat(), 18.0f);

    // name plate
    g.setColour (muted); g.setFont (font (13.0f, false));
    g.drawText ("ours-blanc", 24, 20, 200, 16, juce::Justification::left);
    g.setColour (ink); g.setFont (font (38.0f));
    g.drawText ("Drum Kit", 22, 34, 300, 44, juce::Justification::left);
    const int playing = s.playing.load(), recording = s.recording.load(), events = s.events.load();
    juce::String status = recording ? "Recording - your hits are added to the loop (" + juce::String (events) + ")"
                        : playing   ? "Playing (" + juce::String (events) + " hits)"
                        : events    ? juce::String (events) + " hits in the loop"
                                    : "Hit the pads, play MIDI notes, or record a loop";
    if (s.hostSync.load()) status = "Synced to the host tempo  |  " + status;
    g.setColour (muted); g.setFont (font (13.0f, false));
    g.drawText (status, getWidth() - 520, 52, 496, 20, juce::Justification::right);

    // transport well
    g.setColour (well);
    g.fillRoundedRectangle (ctl[Play].getUnion (ctl[Quant]).getUnion (volume.getBounds()).expanded (6).toFloat(), 12.0f);
    auto key = [&] (juce::Rectangle<int> b, const juce::String& label, bool on, juce::Colour onCol)
    {
        g.setColour (edge); g.fillRoundedRectangle (b.toFloat().translated (0, 2), 8.0f);
        g.setColour (on ? onCol : juce::Colour (0xffEEF0EC)); g.fillRoundedRectangle (b.toFloat(), 8.0f);
        g.setColour (on ? juce::Colours::white : ink); g.setFont (font (14.0f));
        g.drawText (label, b, juce::Justification::centred);
    };
    key (ctl[Play], playing ? "Stop" : "Play", playing, ink);
    key (ctl[Rec], "Rec", recording, rec);
    key (ctl[Clear], "Hold to clear", false, ink);
    if (clearDown >= 0 && ! clearDone)
    {
        const float k = (float) juce::jlimit (0.0, 1.0, (now - clearDown) / 600.0);
        g.setColour (rec); g.fillRect (ctl[Clear].withHeight (3).withY (ctl[Clear].getBottom() - 3).withWidth ((int) (ctl[Clear].getWidth() * k)));
    }
    key (ctl[Click], "Click", s.metronome.load(), ink);
    key (ctl[Quant], "Quantize", s.quantize.load(), ink);
    g.setColour (ink); g.fillRoundedRectangle (bpmBox.toFloat(), 8.0f);
    for (auto c : { BpmDown, BpmUp })
    {
        g.setColour (juce::Colour (0xff33383B)); g.fillRoundedRectangle (ctl[c].toFloat(), 6.0f);
        g.setColour (juce::Colours::white); g.setFont (font (18.0f));
        g.drawText (c == BpmDown ? "-" : "+", ctl[c], juce::Justification::centred);
    }
    g.setColour (lcd); g.setFont (font (22.0f));
    g.drawText (juce::String (s.bpm.load()), bpmBox.withX (bpmBox.getX() + 40).withWidth (50), juce::Justification::centred);
    g.setColour (juce::Colour (0xff9AA39A)); g.setFont (font (11.0f, false));
    g.drawText ("BPM", bpmBox.withX (bpmBox.getX() + 132).withWidth (40), juce::Justification::centred);

    // loop position
    g.setColour (well); g.fillRoundedRectangle (loopBar.toFloat(), 3.0f);
    if (playing)
    {
        g.setColour (recording ? rec : ink);
        g.fillRoundedRectangle (loopBar.withWidth ((int) (loopBar.getWidth() * s.position.load() / DK_LOOP_TICKS)).toFloat(), 3.0f);
    }

    // pads
    for (int i = 0; i < DK_NI; ++i)
    {
        const auto b = pad[i].toFloat();
        const bool lit = now < flashUntil[(size_t) i];
        const auto c = instColour (i);
        g.setColour (edge); g.fillRoundedRectangle (b.translated (0, 3), 12.0f);
        g.setColour (c.darker (0.4f)); g.fillRoundedRectangle (b, 12.0f);
        g.setColour (lit ? c.interpolatedWith (juce::Colours::white, 0.6f) : c.interpolatedWith (juce::Colours::white, 0.18f));
        g.fillRoundedRectangle (b.withTrimmedBottom (5.0f).translated (0, lit ? 3.0f : 0.0f), 12.0f);
        g.setColour (ink); g.setFont (font (18.0f));
        g.drawText (shortName[i], pad[i].reduced (12, 10), juce::Justification::topLeft);
        g.setFont (font (12.0f));
        g.setColour (ink.withAlpha (0.7f));
        g.drawText (juce::String::charToString (keyFor[i]), pad[i].reduced (12, 12), juce::Justification::bottomRight);
    }
    g.setColour (well); g.fillRoundedRectangle (meter.toFloat(), 6.0f);
    const float lv = juce::jlimit (0.0f, 1.0f, s.level.load());
    g.setColour (lv > 0.95f ? rec : juce::Colour (0xff3DBB80));
    g.fillRoundedRectangle (meter.withTrimmedTop ((int) (meter.getHeight() * (1.0f - lv))).toFloat(), 6.0f);

    // sequencer
    g.setColour (seqBg); g.fillRoundedRectangle (seqArea.toFloat(), 14.0f);
    g.setColour (juce::Colours::white); g.setFont (font (16.0f));
    g.drawText ("Sequencer", seqArea.reduced (14).withHeight (22), juce::Justification::left);
    g.setColour (juce::Colour (0xffC9CEC6)); g.setFont (font (12.0f, false));
    g.drawText ("Click a step to add a 16th note. Recorded hits show up here too (2 bars).", seqArea.reduced (14).withHeight (22), juce::Justification::right);
    const int playStep = playing ? (int) (s.position.load() / DK_STEP_TICKS) % DK_STEPS : -1;
    for (int r = 0; r < DK_NI; ++r)
    {
        const int i = order[r];
        const auto first = cellBounds (r, 0);
        g.setColour (instColour (i)); g.setFont (font (11.0f));
        g.drawText (shortName[i], seqArea.getX() + 14, first.getY(), 64, first.getHeight(), juce::Justification::centredLeft);
        const uint32_t mask = s.steps[(size_t) i].load();
        for (int st = 0; st < DK_STEPS; ++st)
        {
            const auto cb = cellBounds (r, st).toFloat();
            g.setColour ((mask >> st) & 1u ? instColour (i) : st % 4 == 0 ? cellBeat : cellOff);
            g.fillRoundedRectangle (cb, 3.0f);
            if (st == playStep) { g.setColour (juce::Colours::white); g.drawRoundedRectangle (cb.reduced (1.0f), 3.0f, 2.0f); }
        }
    }
}

void DrumKitEditor::hitPad (int inst, int vel)
{
    proc.send (DrumKitProcessor::Cmd::Hit, inst, vel);
    flashUntil[(size_t) inst] = juce::Time::getMillisecondCounterHiRes() + 120.0;
}

void DrumKitEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    using C = DrumKitProcessor::Cmd;
    const auto& s = proc.snapshot();
    for (int i = 0; i < DK_NI; ++i)
        if (pad[i].contains (p))
        {   // higher on the pad = harder hit
            const float rel = (p.y - pad[i].getY()) / (float) pad[i].getHeight();
            hitPad (i, juce::jlimit (30, 127, (int) (127.0f - rel * 80.0f)));
            return;
        }
    if (ctl[Play].contains (p)) proc.send (C::TogglePlay);
    else if (ctl[Rec].contains (p)) proc.send (C::ToggleRecord);
    else if (ctl[Clear].contains (p)) { clearDown = juce::Time::getMillisecondCounterHiRes(); clearDone = false; }
    else if (ctl[BpmDown].contains (p)) proc.send (C::SetBpm, s.bpm.load() - (e.mods.isShiftDown() ? 10 : 1));
    else if (ctl[BpmUp].contains (p)) proc.send (C::SetBpm, s.bpm.load() + (e.mods.isShiftDown() ? 10 : 1));
    else if (ctl[Click].contains (p)) proc.send (C::SetMetronome, s.metronome.load() ? 0 : 1);
    else if (ctl[Quant].contains (p)) proc.send (C::SetQuantize, s.quantize.load() ? 0 : 1);
    else if (gridArea.expanded (2).contains (p))
        for (int r = 0; r < DK_NI; ++r)
            for (int st = 0; st < DK_STEPS; ++st)
                if (cellBounds (r, st).expanded (2).contains (p)) { proc.send (C::ToggleStep, order[r], st); return; }
}

void DrumKitEditor::mouseUp (const juce::MouseEvent&) { clearDown = -1.0; }

bool DrumKitEditor::keyPressed (const juce::KeyPress& k)
{
    using C = DrumKitProcessor::Cmd;
    const auto ch = (char) juce::CharacterFunctions::toUpperCase (k.getTextCharacter());
    for (int i = 0; i < DK_NI; ++i)
        if (ch == keyFor[i]) { hitPad (i, k.getModifiers().isShiftDown() ? 127 : 105); return true; }
    if (k == juce::KeyPress::spaceKey) { proc.send (C::TogglePlay); return true; }
    if (ch == 'R') { proc.send (C::ToggleRecord); return true; }
    return false;
}

void DrumKitEditor::timerCallback()
{
    const auto& s = proc.snapshot();
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < DK_NI; ++i)
    {
        const auto h = s.hits[(size_t) i].load();
        if (h != seenHits[(size_t) i]) { seenHits[(size_t) i] = h; flashUntil[(size_t) i] = now + 120.0; }
    }
    if (clearDown >= 0 && ! clearDone && now - clearDown >= 600.0)
    {
        proc.send (DrumKitProcessor::Cmd::Clear);
        clearDone = true;
    }
    repaint();
}
