#!/usr/bin/env bash
# Compiles PluginSettings.swf with the Apache Flex SDK 4.16.1 compiler and copies it to
# ../plugin_settings.swf. The xScal build turns that file into a byte array for
# plugin_settings_menu.cpp (see CMakeLists.txt). Run it after changing src/ or assets/ and commit
# plugin_settings.swf; the xScal build itself does not need the Flex SDK.
#
#   FLEX_HOME   Flex SDK root (default: ~/sdks/flex-4.16.1). Needs
#               frameworks/libs/player/32.0/playerglobal.swc.
set -euo pipefail
cd "$(dirname "$0")"

FLEX_HOME="${FLEX_HOME:-$HOME/sdks/flex-4.16.1}"
export FLEX_HOME
# flex-config.xml resolves {playerglobalHome} from this variable.
export PLAYERGLOBAL_HOME="${PLAYERGLOBAL_HOME:-$FLEX_HOME/frameworks/libs/player}"

COMMON=(-target-player=32.0 -swf-version=19 -debug=false -optimize=true
        -compiler.actionscript-file-encoding=UTF-8
        -static-link-runtime-shared-libraries=true)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The game's MenuComponent and its bases are compiled to a SWC that mxmlc links as external, so the
# game's own classes are used at run time.
"$FLEX_HOME/bin/compc" "${COMMON[@]}" \
    -source-path externs \
    -include-sources externs \
    -output "$work/externs.swc" \
    > "$work/compc.log" 2>&1 || { cat "$work/compc.log"; exit 1; }

"$FLEX_HOME/bin/mxmlc" "${COMMON[@]}" \
    -source-path src \
    -external-library-path+="$work/externs.swc" \
    -output "$work/PluginSettings.swf" \
    src/PluginSettings.as \
    > "$work/mxmlc.log" 2>&1 || { cat "$work/mxmlc.log"; exit 1; }

# mxmlc stores every embedded PNG with 32 bits per pixel. The credits pictures have at most 256
# colors each; stored as 8-bit indexes they take about a third of the space.
python3 -I index_bitmaps.py "$work/PluginSettings.swf"

cp "$work/PluginSettings.swf" ../plugin_settings.swf
echo "plugin_settings.swf: $(grep -c 'Warning' "$work/mxmlc.log" || true) warnings, $(stat -c %s "$work/PluginSettings.swf") bytes"
