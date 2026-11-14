#pragma once

#include "xscal/plugin_api.h"

namespace sf::jsonapi {

// The xscal.json interface handed to plugins. It wraps the yyjson copy that
// xScal is built with and has no other dependency, so plugin tests can link
// this file and yyjson.c to get the same table without a host.
[[nodiscard]] const XScalJsonV1* Interface() noexcept;

}  // namespace sf::jsonapi
