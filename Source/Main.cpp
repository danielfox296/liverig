#include <juce_gui_basics/juce_gui_basics.h>

#include "AudioEngine.h"
#include "MainComponent.h"
#include "Parameters.h"
#include "SettingsStore.h"
#include "ui/Widgets.h"

namespace rig
{

class LiveRigApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "LiveRig"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    // One instance on stage — but the diagnostic must be able to run beside
    // a live session, because "it doesn't work" is exactly when it runs.
    bool moreThanOneInstanceAllowed() override
    {
        return getCommandLineParameters().contains ("--diagnose");
    }

    void initialise (const juce::String&) override
    {
       #if JUCE_IOS
        useTouchLayout();   // before any component exists
       #endif

        // Launched from the Dock or Finder, an app inherits a bare PATH. The
        // stem splitter shells out to ffmpeg, so give its children a real one.
        setenv ("PATH", "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);

        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        settings.load (params);
        engine.getFoot().setMap (FootMap::loadFrom (FootMap::getFile()));

        std::unique_ptr<juce::XmlElement> deviceState;

        if (settings.deviceState.isNotEmpty())
            deviceState = juce::parseXML (settings.deviceState);

        const auto error = engine.initialise (std::move (deviceState));

        const bool diagnosing = getCommandLineParameters().contains ("--diagnose");

        if (error.isNotEmpty() && ! diagnosing)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Audio device", error);

        window = std::make_unique<MainWindow> (params, engine, settings);

        // `LiveRig --diagnose` from a terminal: the full real startup — same
        // binary, same settings, same device — then the facts and out. This
        // exists because "nothing happens" must always be answerable.
        if (diagnosing)
        {
            const auto say = [] (const char* fmt, auto... args)
            { std::fprintf (stderr, fmt, args...); };

            say ("LiveRig diagnose\n================\n");

            if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
                say ("device: %s | %.0f Hz | %d samples | %d in / %d out\n",
                     device->getName().toRawUTF8(),
                     device->getCurrentSampleRate(),
                     device->getCurrentBufferSizeSamples(),
                     device->getActiveInputChannels().countNumberOfSetBits(),
                     device->getActiveOutputChannels().countNumberOfSetBits());
            else
                say ("device: NONE OPEN%s%s\n", error.isEmpty() ? "" : " — ",
                     error.toRawUTF8());

            {
                const auto foot = engine.getFoot().getMap();
                say ("foot: %d controls | layout \"%s\" of %d | listening to %s\n",
                     (int) foot.controls.size(), foot.active.toRawUTF8(), (int) foot.layouts.size(),
                     foot.device.isEmpty() ? "any controller" : foot.device.toRawUTF8());
            }

            if (const double rate = engine.getSampleRate(); rate > 0.0)
                say ("looper: takes a played note to arrive %d samples (%.1f ms) late, plus its board\n",
                     engine.getLoopRoundTripSamples(), engine.getLoopRoundTripSamples() / rate * 1000.0);

            say ("mic path: %s\n", engine.getActiveInputCount() >= 2
                                       ? "input 2 available"
                                       : "NO INPUT 2 on this device — the mic lane is silent");

            say ("registry: %d effects, %d instruments cached\n",
                 engine.getInstrument().getEffectNames().size(),
                 engine.getInstrument().getAvailableNames().size());

            say ("preferred pitch plugin: %s\n",
                 engine.getInstrument().preferredPitchPlugin().toRawUTF8());

            auto& tune = engine.getTuneSlot();
            say ("tune slot: %s\n",
                 tune.isLoaded()
                     ? (tune.getLoadedName() + " loaded, " + juce::String (tune.getLatencyMs(), 1)
                          + " ms latency").toRawUTF8()
                     : (tune.getLastError().isNotEmpty() ? tune.getLastError().toRawUTF8()
                                                         : "EMPTY, no error recorded"));

            auto& vocoder = engine.getVocoderSlot();
            say ("vocoder slot: %s\n",
                 vocoder.isLoaded()
                     ? (vocoder.getLoadedName() + " loaded, " + juce::String (vocoder.getLatencyMs(), 1)
                          + " ms latency, "
                          + (vocoder.getSidechainChannels() > 0
                                 ? "carrier on the sidechain (" + juce::String (vocoder.getSidechainChannels()) + "ch)"
                                 : juce::String ("carrier on the right main channel"))).toRawUTF8()
                     : (vocoder.getLastError().isNotEmpty() ? vocoder.getLastError().toRawUTF8()
                                                            : "EMPTY, no error recorded"));

            say ("vocoder switch: %s | level %.1f dB\n",
                 params.getBool (pVocoderOn) ? "ON" : "off", params.get (pVocoderLevel));

            say ("autotune switch: %s | vocal gain %.1f dB, gate %.1f dB, level %.1f dB\n",
                 params.getBool (pVocalOn) ? "ON" : "off",
                 params.get (pVocalGain), params.get (pVocalGate), params.get (pVocalLevel));

            // The editor needs the message loop to lay itself out — Melda's
            // report 1x1 until then — so it is measured in a real window
            // after the loop has run, exactly the way a person meets it.
            if (auto* plugin = tune.getLiveProcessor())
            {
                if (auto* editor = plugin->createEditorIfNeeded())
                {
                    probeWindow = std::make_unique<juce::DocumentWindow> (
                        "probe", juce::Colours::black, juce::DocumentWindow::closeButton);
                    probeWindow->setUsingNativeTitleBar (true);

                    if (editor->getWidth() < 40 || editor->getHeight() < 40)
                        editor->setSize (560, 400);

                    probeWindow->setContentOwned (editor, true);
                    probeWindow->setVisible (true);

                    juce::Timer::callAfterDelay (800, [this]
                    {
                        auto* content = probeWindow->getContentComponent();
                        std::fprintf (stderr, "tune editor after layout: %dx%d\n",
                                      content != nullptr ? content->getWidth() : 0,
                                      content != nullptr ? content->getHeight() : 0);
                        probeWindow.reset();
                        quit();
                    });

                    return;
                }

                say ("tune editor: REFUSED — Key & style... would do nothing\n");
            }

            quit();
        }
    }

    void shutdown() override
    {
        if (window != nullptr)
        {
            window->saveSettings();
            window.reset();
        }

        engine.shutdown();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

    // iOS can end a backgrounded app without calling shutdown: save on the
    // way out of the foreground.
    void suspended() override
    {
        if (window != nullptr)
            window->saveSettings();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (ParamStore& p, AudioEngine& e, SettingsStore& s)
            : DocumentWindow ("LiveRig",
                              juce::Colour (0xff101114),
                              DocumentWindow::allButtons)
        {
            auto* content = new MainComponent (p, e, s);
            main = content;

            setUsingNativeTitleBar (true);
            setContentOwned (content, true);
            setResizable (true, false);
            setResizeLimits (860, 560, 2400, 1800);
            int height = getHeight();

            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                height = juce::jmin (height, display->userArea.getHeight() - 60);

            centreWithSize (getWidth(), height);
            setVisible (true);
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }

        void saveSettings() { if (main != nullptr) main->saveSettings(); }

    private:
        MainComponent* main = nullptr;
    };

    ParamStore params;
    SettingsStore settings;
    AudioEngine engine { params };
    RigLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<juce::DocumentWindow> probeWindow;   // --diagnose only
};

} // namespace rig

// START_JUCE_APPLICATION, unrolled: the scan worker must be intercepted
// before any application machinery — no window, no audio, no Dock presence —
// because its whole job is to die in the rig's place.
JUCE_CREATE_APPLICATION_DEFINE (rig::LiveRigApplication)

int main (int argc, char* argv[])
{
    if (argc >= 4 && juce::String (argv[1]) == "--scan-worker")
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        rig::InstrumentHost::runScanWorker (juce::String (argv[2]), juce::String (argv[3]));
    }

    // What START_JUCE_APPLICATION's main actually does before handing over:
    // register the application factory. Skip it and main() creates the app
    // through a null pointer and dies before its first line of output.
    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;

    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
