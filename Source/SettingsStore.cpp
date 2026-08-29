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
        tunePlugin = obj->getProperty ("tunePlugin").toString();
        tunePluginState = obj->getProperty ("tunePluginState").toString();
        pedalPlugin = obj->getProperty ("pedalPlugin").toString();
        pedalPluginState = obj->getProperty ("pedalPluginState").toString();
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
    root->setProperty ("tunePlugin", tunePlugin);
    root->setProperty ("tunePluginState", tunePluginState);
    root->setProperty ("pedalPlugin", pedalPlugin);
    root->setProperty ("pedalPluginState", pedalPluginState);

    const auto file = getFile();
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (root), false));
}

} // namespace rig
