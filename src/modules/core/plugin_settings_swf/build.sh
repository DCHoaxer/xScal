#!/usr/bin/env bash
# Compiles PluginSettings.swf with the Apache Flex SDK 4.16.1 compiler and writes its bytes to
# ../plugin_settings_swf_bytes.inc, which plugin_settings_menu.cpp embeds. Run it after changing
# src/ and commit the generated file; the xScal build itself does not need the Flex SDK.
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

od -An -v -tu1 "$work/PluginSettings.swf" | sed -E 's/ +/,/g; s/^,//; s/$/,/' > ../plugin_settings_swf_bytes.inc
echo "PluginSettings.swf: $(grep -c 'Warning' "$work/mxmlc.log" || true) warnings, $(stat -c %s "$work/PluginSettings.swf") bytes"
