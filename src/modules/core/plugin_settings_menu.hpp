#pragma once

namespace sf::modules {

// Registers the patch that loads PluginSettings.swf into seventysixmenu.swf. The SWF adds the
// PLUGINS SETTINGS entry to the main menu and lists the options plugins provide through
// xscal.plugin-options. Call once, after the movie interface can take handlers.
void InstallPluginSettingsMenu() noexcept;

}  // namespace sf::modules
