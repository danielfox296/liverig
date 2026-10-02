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

// A button with no text is invisible to the person using it. Catch it here
// rather than in front of an audience.
static int countBlankButtons (juce::Component& parent, const juce::String& context)
{
    int blanks = 0;

    for (auto* child : parent.getChildren())
    {
        if (! child->isVisible())
            continue;

        if (auto* button = dynamic_cast<juce::TextButton*> (child))
        {
            if (button->getButtonText().trim().isEmpty())
            {
                ++blanks;
                std::printf ("  BLANK BUTTON in %s at %d,%d %dx%d\n",
                             context.toRawUTF8(),
                             button->getX(), button->getY(),
                             button->getWidth(), button->getHeight());
            }
        }

        blanks += countBlankButtons (*child, context);
    }

    return blanks;
}

static void writePng (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::PNGImageFormat png;
    png.writeImageToStream (image, stream);
    std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
}

static int problems = 0;

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
        settings.channelList = "bass,mic,keys";   // stage the full rail the snapshots document
        settings.bassLoadout = "midi,comp,eq,filter,boost,drive,amp,cab,octave,sub,synth,"
                               "phaser,chorus,delay,tape,freeze,reverb";
        settings.micLoadout = "vocal,vocoder";
        settings.keysLoadout = "keys";
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
        problems += countBlankButtons (panel, "the new-song dialog");

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
        problems += countBlankButtons (installPanel, "the install prompt");

        // A background job running: the window stays fully usable behind it.
        main.showJobStripForSnapshot();

        juce::Image working (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (working);
            main.paintEntireComponent (g, true);
        }
        writePng (working, outDir.getChildFile ("job-running.png"));
        problems += countBlankButtons (main, "the main window");

        const int instruments = main.getInstrumentChoiceCount();
        std::printf ("  instrument picker offers %d\n", instruments);

        if (instruments < 1)
        {
            std::printf ("  EMPTY INSTRUMENT PICKER — nothing selectable\n");
            ++problems;
        }

        // An effect selected, to check the editor panel lays its controls out.
        main.selectFxForSnapshot();

        juce::Image expanded (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (expanded);
            main.paintEntireComponent (g, true);
        }
        writePng (expanded, outDir.getChildFile ("fx-editor.png"));

        // The EQ's curve editor, with a shaped curve so every element shows.
        params.set (rig::pEqOn, 1.0f);
        params.set (rig::pEq1Freq, 70.0f);   params.set (rig::pEq1Gain, 3.0f);
        params.set (rig::pEq2Freq, 180.0f);  params.set (rig::pEq2Gain, -2.5f);  params.set (rig::pEq2Q, 1.4f);
        params.set (rig::pEq3Freq, 700.0f);  params.set (rig::pEq3Gain, 2.0f);
        params.set (rig::pEq4Freq, 2600.0f); params.set (rig::pEq4Gain, 4.5f);   params.set (rig::pEq4Q, 2.2f);
        params.set (rig::pEq5Freq, 9000.0f); params.set (rig::pEq5Gain, -3.0f);
        main.selectEqForSnapshot();

        juce::Image eqShot (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (eqShot);
            main.paintEntireComponent (g, true);
        }
        writePng (eqShot, outDir.getChildFile ("eq-editor.png"));
        problems += countBlankButtons (main, "the EQ editor");

        // The freeze pedal, held: its hold switch, level, fades and motion.
        params.set (rig::pFreezeOn, 1.0f);
        params.set (rig::pFreezeHold, 1.0f);
        main.selectFreezeForSnapshot();

        juce::Image freezeShot (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (freezeShot);
            main.paintEntireComponent (g, true);
        }
        writePng (freezeShot, outDir.getChildFile ("freeze.png"));
        problems += countBlankButtons (main, "the freeze editor");
        params.set (rig::pFreezeHold, 0.0f);

        // The autotune strip: the switch, its three faders, and the status line.
        params.set (rig::pVocalOn, 1.0f);
        main.selectVocalForSnapshot();

        juce::Image vocalShot (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (vocalShot);
            main.paintEntireComponent (g, true);
        }
        writePng (vocalShot, outDir.getChildFile ("vocal.png"));
        problems += countBlankButtons (main, "the vocal editor");

        // The vocoder strip beside it: switch, shared mic controls, status.
        params.set (rig::pVocoderOn, 1.0f);
        main.selectVocoderForSnapshot();

        juce::Image vocoderShot (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (vocoderShot);
            main.paintEntireComponent (g, true);
        }
        writePng (vocoderShot, outDir.getChildFile ("vocoder.png"));
        problems += countBlankButtons (main, "the vocoder editor");

        // The keys strip: which instrument it plays and how loud, which is
        // the whole of that row now the tracker has gone back to the bass.
        main.selectKeysForSnapshot();

        juce::Image keysShot (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (keysShot);
            main.paintEntireComponent (g, true);
        }
        writePng (keysShot, outDir.getChildFile ("keys.png"));
        problems += countBlankButtons (main, "the keys editor");

        // The tuner tab, mid-note.
        main.showTunerForSnapshot();

        juce::Image tuner (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (tuner);
            main.paintEntireComponent (g, true);
        }
        writePng (tuner, outDir.getChildFile ("tuner.png"));

        // The foot tab: controls, their jobs, and the last thing heard.
        main.showFootForSnapshot();

        juce::Image foot (juce::Image::ARGB, main.getWidth(), main.getHeight(), true);
        {
            juce::Graphics g (foot);
            main.paintEntireComponent (g, true);
        }
        writePng (foot, outDir.getChildFile ("foot.png"));
        problems += countBlankButtons (main, "the foot tab");
        problems += main.checkFootFunctionsForSnapshot();
        problems += main.exerciseFootForSnapshot();
    }

    {
        // A fresh rig: no settings file yet, so the rail is one bass strip,
        // the master, and the + — the state a new player meets.
        rig::ParamStore freshParams;
        rig::SettingsStore freshSettings;
        rig::AudioEngine freshEngine (freshParams);
        rig::MainComponent fresh (freshParams, freshEngine, freshSettings);
        fresh.selectInputForSnapshot();   // the INPUT editor and its source picker

        juce::Image shot (juce::Image::ARGB, fresh.getWidth(), fresh.getHeight(), true);
        {
            juce::Graphics g (shot);
            fresh.paintEntireComponent (g, true);
        }
        writePng (shot, outDir.getChildFile ("fresh.png"));
        problems += countBlankButtons (fresh, "the fresh window");
    }

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);

    std::printf ("%s\n", problems == 0 ? "no blank controls"
                                        : "FOUND UNLABELLED CONTROLS");
    return problems == 0 ? 0 : 1;
}
