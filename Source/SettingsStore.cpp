#include "SettingsStore.h"

namespace rig
{

juce::File SettingsStore::getFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
              .getChildFile ("Application Support")
              .getChildFile ("LiveRig")
              .getChildFile ("settings.json");
}

void SettingsStore::load (ParamStore& params)
{
    const auto file = getFile();

    if (! file.existsAsFile())
        return;

    const auto parsed = juce::JSON::parse (file.loadFileAsString());

    if (auto* obj = parsed.getDynamicObject())
    {
        params.fromVar (obj->getProperty ("params"));
        songFolder = obj->getProperty ("songFolder").toString();
        deviceState = obj->getProperty ("audioDeviceState").toString();
        expandedPanels = obj->getProperty ("expandedPanels").toString();
        instrument = obj->getProperty ("instrument").toString();
        instrumentState = obj->getProperty ("instrumentState").toString();
        chainOrder = obj->getProperty ("chainOrder").toString();
        tuner = obj->getProperty ("tuner").toString();
        ampModel = obj->getProperty ("ampModel").toString();
        bassInput = (int) obj->getProperty ("bassInput");
        micInput = (int) obj->getProperty ("micInput");
        bassName = obj->getProperty ("bassName").toString();
        micName = obj->getProperty ("micName").toString();
        keysName = obj->getProperty ("keysName").toString();

        const auto storedChannels = obj->getProperty ("channelList").toString();
        channelList = storedChannels.isNotEmpty() ? storedChannels
                    : instrument.isNotEmpty()     ? "bass,mic,keys"
                                                  : "bass,mic";

        if (obj->hasProperty ("bassLoadout"))
        {
            bassLoadout = obj->getProperty ("bassLoadout").toString();
            micLoadout = obj->getProperty ("micLoadout").toString();
            keysLoadout = obj->getProperty ("keysLoadout").toString();
            extraChannels = obj->getProperty ("extraChannels").toString();
            midiSource = obj->getProperty ("midiSource").toString();
        }
        else
        {
            // A file from before loadouts showed every row; keep showing them.
            bassLoadout = "comp,eq,filter,boost,drive,amp,cab,octave,sub,synth,"
                          "phaser,chorus,delay,tape,reverb";
            micLoadout = "vocal,vocoder";
            keysLoadout = "midi";
        }

        // The loadout's sequence is the board's wiring now. A file whose
        // wiring still lives in the old whole-chain order string hands that
        // sequence to the bass loadout; stages the order knows come first,
        // in its order, and anything else keeps its place after them.
        if (chainOrder.isNotEmpty())
        {
            auto loaded = juce::StringArray::fromTokens (bassLoadout, ",", "");
            loaded.trim();
            juce::StringArray reordered;

            for (const auto& key : juce::StringArray::fromTokens (chainOrder, ",", ""))
                if (loaded.contains (key.trim()))
                    reordered.add (key.trim());

            for (const auto& key : loaded)
                if (! reordered.contains (key))
                    reordered.add (key);

            bassLoadout = reordered.joinIntoString (",");
        }
        tunePlugin = obj->getProperty ("tunePlugin").toString();
        tunePluginState = obj->getProperty ("tunePluginState").toString();
        vocoderPlugin = obj->getProperty ("vocoderPlugin").toString();
        vocoderPluginState = obj->getProperty ("vocoderPluginState").toString();
    }
}

void SettingsStore::save (const ParamStore& params) const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("params", params.toVar());
    root->setProperty ("songFolder", songFolder);
    root->setProperty ("audioDeviceState", deviceState);
    root->setProperty ("expandedPanels", expandedPanels);
    root->setProperty ("instrument", instrument);
    root->setProperty ("instrumentState", instrumentState);
    root->setProperty ("chainOrder", chainOrder);
    root->setProperty ("tuner", tuner);
    root->setProperty ("ampModel", ampModel);
    root->setProperty ("bassInput", bassInput);
    root->setProperty ("micInput", micInput);
    root->setProperty ("bassName", bassName);
    root->setProperty ("micName", micName);
    root->setProperty ("keysName", keysName);
    root->setProperty ("channelList", channelList);
    root->setProperty ("bassLoadout", bassLoadout);
    root->setProperty ("micLoadout", micLoadout);
    root->setProperty ("keysLoadout", keysLoadout);
    root->setProperty ("extraChannels", extraChannels);
    root->setProperty ("midiSource", midiSource);
    root->setProperty ("tunePlugin", tunePlugin);
    root->setProperty ("tunePluginState", tunePluginState);
    root->setProperty ("vocoderPlugin", vocoderPlugin);
    root->setProperty ("vocoderPluginState", vocoderPluginState);

    const auto file = getFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (root), false));
}

} // namespace rig
