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
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        // Launched from the Dock or Finder, an app inherits a bare PATH. The
        // stem splitter shells out to ffmpeg, so give its children a real one.
        setenv ("PATH", "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);

        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        settings.load (params);

        std::unique_ptr<juce::XmlElement> deviceState;

        if (settings.deviceState.isNotEmpty())
            deviceState = juce::parseXML (settings.deviceState);

        const auto error = engine.initialise (std::move (deviceState));

        if (error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Audio device", error);

        window = std::make_unique<MainWindow> (params, engine, settings);
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
};

} // namespace rig

START_JUCE_APPLICATION (rig::LiveRigApplication)
