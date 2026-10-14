#pragma once

#include "scaleform/abi.hpp"
#include "xscal/plugin_api.h"

#include <cstdint>

namespace sf {
struct MovieRootContext;
}

// The movie handle passed to plugins. xScal code can make one for a context it
// already has and use the same interface, for example through xscal::Movie.
struct XScalMovie final {
    const sf::MovieRootContext* context;
};

namespace sf::movieapi {

// Process-lifetime table returned for XSCAL_SCALEFORM_MOVIE_INTERFACE. Version
// 1 callers read its XScalScaleformMovieV1 prefix.
[[nodiscard]] const XScalScaleformMovieV2* Interface() noexcept;
// The same functions as Interface(), with version 1 and the version 1 size.
// Plugins built with SDK 5 may compare the version for equality.
[[nodiscard]] const XScalScaleformMovieV1* InterfaceV1() noexcept;

// Passes one MovieRoot::GetVariable call to the plugin lookup handlers.
// Runs on the calling game thread after the original GetVariable returned.
void NotifyLookup(
    const MovieRootContext& context,
    const char* path) noexcept;

// Called for every movie on every AdvanceFrame, after the original returned.
// Returns at once when no plugin uses movie handlers, invoke filters, posted
// work or advance watches. Otherwise runs posted work, detects that root1
// exists, tells matching movie handlers about the loaded movie and calls the
// advance handlers when the movie is watched.
void AdvanceMovie(void* movieRoot) noexcept;
// Called when a MovieRoot is about to be destroyed, before ForgetMovie. Calls
// the unloading handlers of a loaded movie, releases the values plugins
// retained from it, and drops its posted work and movie record.
void ReleaseMovie(void* movieRoot) noexcept;
// Passes one MovieRoot::Invoke call to the plugin invoke filters. Returns false
// when a filter asked to drop the call. arguments uses the game's
// layout::kValueArrayStride stride.
[[nodiscard]] bool AllowInvoke(
    void* movieRoot,
    const char* methodPath,
    const ScaleformValue* arguments,
    std::uint32_t argumentCount) noexcept;
// True when swfName is NULL or equals the file name part of url (after the
// last '/' or '\\'), ignoring ASCII case.
[[nodiscard]] bool MatchesSwfName(const char* swfName, const char* url) noexcept;
// Ends every watch on a MovieRoot that is being destroyed, so a later movie
// at the same address is not reported to plugins.
void ForgetMovie(void* movieRoot) noexcept;
// Set by the hook controller when the AdvanceFrame slot is hooked.
void SetAdvanceAvailable(bool available) noexcept;
// Set by the hook controller when the Invoke slot is hooked.
void SetInvokeAvailable(bool available) noexcept;

// Removes all handlers and functions. Tests only.
void ResetForTests() noexcept;

}
