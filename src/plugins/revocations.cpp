#include "plugins/plugin_signature.hpp"

namespace sf::plugins::signing {

// Developer certificates revoked for breaking the SDK rules. Every plugin
// signed with one of these certificates is rejected.
std::span<const std::uint32_t> RevokedDeveloperIds() noexcept {
    return {};
}

// Single builds revoked from developers who are otherwise in good standing.
// Get a build's SHA-256 with "xscal-sign hash <dll>".
std::span<const Hash> RevokedPluginHashes() noexcept {
    return {};
}

}  // namespace sf::plugins::signing
