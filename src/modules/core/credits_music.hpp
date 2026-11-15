#pragma once

namespace sf::modules {

// Plays the game's Nuka theme as the main menu music while the xScal credits are open, and the
// menu's own track again after they close. Called on every seventysixmenu.swf frame with the
// credits state; only a change of the state does anything.
void SetCreditsMusicPlaying(bool playing) noexcept;

}  // namespace sf::modules
