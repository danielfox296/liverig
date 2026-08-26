#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace rig
{

// Every piece of type in the app is sized through here, so the whole face
// can be scaled in one place.
constexpr float kTypeScale = 1.1f;

inline juce::Font rigFont (float size, bool bold = false)
{
    return juce::Font (juce::FontOptions (size * kTypeScale,
                                          bold ? juce::Font::bold : juce::Font::plain));
}

} // namespace rig
