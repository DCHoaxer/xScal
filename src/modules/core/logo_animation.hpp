#pragma once

namespace sf::modules {

// Name of the version text block on root1.Menu_mc of mainmenu.swf. CoreModule adds it hidden and the
// logo animation shows it.
inline constexpr char kVersionBlockName[] = "xScalInfo_mc";

// Registers the patch that plays the logo animation at the bottom left of mainmenu.swf and then shows
// the version text block. Call once, after the movie interface can take handlers.
void InstallLogoAnimation() noexcept;

}  // namespace sf::modules
