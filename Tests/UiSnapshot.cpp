// Renders the window offscreen to PNG files so the layout can be checked
// without a person having to look at a running app.
//
//   rig_uishot <output-directory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "AudioEngine.h"
#include "MainComponent.h"
#include "Parameters.h"
#include "SettingsStore.h"
#include "SongMaker.h"
#include "ui/SongMakerPanel.h"
#include "ui/Widgets.h"

static void writePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    png.writeImageToStream (image, stream);
    std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
}

int main (int argc, char* argv[])
{
    const juce::File outDir (argc > 1 ? juce::String (argv[1])
                                      : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();

    juce::ScopedJuceInitialiser_GUI juceInit;

    rig::RigLookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    {
        rig::ParamStore params;
        rig::SettingsStore settings;
        rig::AudioEngine engine (params);          // deliberately not opened
        rig::MainComponent main (params, engine, settings);   // sizes itself

        juce::Image shot (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (shot);
            main.paintEntireComponent (g, true);
        }
        writePng (shot, outDir.getChildFile ("window.png"));

        // The song pipeline, over the top of the same window.
        rig::SongMaker maker;
        rig::SongMakerPanel panel (maker);
        panel.setSize (main.getWidth(), main.getHeight());
        panel.confirmSong (juce::File ("/Users/fox296/Downloads/Plan B.mp3"));
        panel.setBounds (0, 0, main.getWidth(), main.getHeight());

        juce::Image withPanel (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (withPanel);
            main.paintEntireComponent (g, true);
            panel.paintEntireComponent (g, true);
        }
        writePng (withPanel, outDir.getChildFile ("new-song.png"));

        rig::SongMakerPanel installPanel (maker);
        installPanel.setBounds (0, 0, main.getWidth(), main.getHeight());
        installPanel.offerInstall();

        juce::Image withInstall (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (withInstall);
            main.paintEntireComponent (g, true);
            installPanel.paintEntireComponent (g, true);
        }
        writePng (withInstall, outDir.getChildFile ("install-prompt.png"));

        // A background job running: the window stays fully usable behind it.
        main.showJobStripForSnapshot();

        juce::Image working (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (working);
            main.paintEntireComponent (g, true);
        }
        writePng (working, outDir.getChildFile ("job-running.png"));

        // Everything open, to check the effect list scrolls rather than clips.
        main.expandAllForSnapshot();

        juce::Image expanded (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (expanded);
            main.paintEntireComponent (g, true);
        }
        writePng (expanded, outDir.getChildFile ("all-expanded.png"));
    }

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    return 0;
}
