cmake_minimum_required(VERSION 3.20)

project(xscal LANGUAGES C CXX ASM_MASM)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "xscal requires a Windows x64 toolchain")
endif()

# The plugin SDK headers, yyjson and libdeflate are not part of this repository.
set(XSCAL_SDK_INCLUDE_DIR "" CACHE PATH
    "Directory containing the xScal plugin SDK headers (xscal/plugin_api.h)")
set(XSCAL_YYJSON_DIR "" CACHE PATH
    "Directory containing yyjson.c and yyjson.h")
set(XSCAL_LIBDEFLATE_DIR "" CACHE PATH
    "Directory containing libdeflate.h and lib/ of libdeflate 1.22")

if(NOT EXISTS "${XSCAL_SDK_INCLUDE_DIR}/xscal/plugin_api.h")
    message(FATAL_ERROR "Set XSCAL_SDK_INCLUDE_DIR to the xScal plugin SDK include directory")
endif()
if(NOT EXISTS "${XSCAL_YYJSON_DIR}/yyjson.c")
    message(FATAL_ERROR "Set XSCAL_YYJSON_DIR to a yyjson source directory")
endif()
if(NOT EXISTS "${XSCAL_LIBDEFLATE_DIR}/lib/deflate_decompress.c")
    message(FATAL_ERROR "Set XSCAL_LIBDEFLATE_DIR to a libdeflate source directory")
endif()

# Only the zlib decompressor of libdeflate is built. It is compiled for speed in
# every configuration because it decompresses every compressed archive entry.
set(XSCAL_LIBDEFLATE_SOURCES
    "${XSCAL_LIBDEFLATE_DIR}/lib/adler32.c"
    "${XSCAL_LIBDEFLATE_DIR}/lib/deflate_decompress.c"
    "${XSCAL_LIBDEFLATE_DIR}/lib/utils.c"
    "${XSCAL_LIBDEFLATE_DIR}/lib/x86/cpu_features.c"
    "${XSCAL_LIBDEFLATE_DIR}/lib/zlib_decompress.c"
)

set(XSCAL_CHAT_SOURCES
    src/chat/chat_auth_file.cpp
    src/chat/chat_callbacks.cpp
    src/chat/chat_interface.cpp
    src/chat/fcm_log.cpp
    src/chat/chat_session.cpp
    src/chat/chat_types.cpp
    src/chat/relay_connection.cpp
    src/chat/relay_protocol.cpp
    src/chat/scaleform_payload.cpp
)

set(XSCAL_CORE_SOURCES
    ${XSCAL_CHAT_SOURCES}
    "${XSCAL_YYJSON_DIR}/yyjson.c"
    ${XSCAL_LIBDEFLATE_SOURCES}
    src/address/address_library.cpp
    src/archive/archive_stream_hook.cpp
    src/common/json.cpp
    src/api/mod_storage.cpp
    src/api/file_command_service.cpp
    src/api/internal/enemy_meter_data.cpp
    src/api/internal/enemy_meter_native.cpp
    src/api/internal/stealth_meter_data.cpp
    src/api/scaleform_callbacks.cpp
    src/api/scaleform_movie_interface.cpp
    src/input/input_state.cpp
    src/datamanager/pip_boy_stats_effects_provider.cpp
    src/datamanager/active_effect_snapshot.cpp
    src/datamanager/player_map_position_provider.cpp
    src/datamanager/player_map_position_snapshot.cpp
    src/datamanager/time_data_provider.cpp
    src/datamanager/time_data_snapshot.cpp
    src/datamanager/actor_value_reader.cpp
    src/datamanager/local_player.cpp
    src/datamanager/game_string.cpp
    src/datamanager/game_read_lock.cpp
    src/datamanager/json_result_cache.cpp
    src/datamanager/form_text.cpp
    src/datamanager/pip_boy_status_provider.cpp
    src/datamanager/pip_boy_status_snapshot.cpp
    src/datamanager/pip_boy_player_info_provider.cpp
    src/datamanager/pip_boy_player_info_snapshot.cpp
    src/datamanager/pip_boy_perks_provider.cpp
    src/datamanager/pip_boy_perks_snapshot.cpp
    src/datamanager/pip_boy_quests_provider.cpp
    src/datamanager/pip_boy_quests_snapshot.cpp
    src/datamanager/pip_boy_inventory_provider.cpp
    src/datamanager/pip_boy_inventory_snapshot.cpp
    src/datamanager/pip_boy_collections_provider.cpp
    src/datamanager/pip_boy_collections_snapshot.cpp
    src/datamanager/pip_boy_radio_provider.cpp
    src/datamanager/pip_boy_radio_snapshot.cpp
    src/datamanager/pip_boy_special_provider.cpp
    src/datamanager/pip_boy_special_snapshot.cpp
    src/platform/diagnostics.cpp
    src/platform/native_access.cpp
    src/platform/win32_file.cpp
    src/plugins/plugins.cpp
    src/plugins/plugin_signature.cpp
    src/hook/movie_root_hook.cpp
    src/hook/movie_root_router.cpp
    src/input/gamepad_suppression.cpp
    src/input/xinput_hook.cpp
    src/modules/core/core_module.cpp
    src/modules/xscal_priority/xscal_priority.cpp
    src/platform/windows_memory.cpp
    src/runtime/runtime.cpp
    src/scaleform/value.cpp
    src/scaleform/bridge_attachment.cpp
    src/scaleform/native_handler.cpp
    src/config/config.cpp
    src/config/target_profile.cpp
    src/hook/vtable_hook.cpp
)

set(XSCAL_SEH_SOURCES
    src/api/internal/enemy_meter_native.cpp
    src/api/scaleform_movie_interface.cpp
    src/datamanager/active_effect_snapshot.cpp
    src/hook/movie_root_router.cpp
    src/input/xinput_hook.cpp
    src/platform/native_access.cpp
    src/plugins/plugins.cpp
    src/scaleform/native_handler.cpp
)
set_source_files_properties(${XSCAL_SEH_SOURCES} PROPERTIES
    COMPILE_OPTIONS /EHa)

# Generates xscal_build_timestamp.hpp on every build.
set(XSCAL_BUILD_INFO_DIR "${CMAKE_BINARY_DIR}/generated")
set(XSCAL_BUILD_INFO_HEADER
    "${XSCAL_BUILD_INFO_DIR}/xscal_build_timestamp.hpp")
set(XSCAL_BUILD_INFO_SCRIPT
    "${CMAKE_BINARY_DIR}/write_build_timestamp.cmake")
file(WRITE "${XSCAL_BUILD_INFO_SCRIPT}" [=[
string(TIMESTAMP XSCAL_BUILD_DATE "%Y-%m-%d")
string(TIMESTAMP XSCAL_BUILD_TIME "%H:%M:%S")
file(WRITE "${OUTPUT}"
    "#pragma once\n\n"
    "namespace sf::build_info {\n"
    "inline constexpr char kDate[]{\"${XSCAL_BUILD_DATE}\"};\n"
    "inline constexpr char kTime[]{\"${XSCAL_BUILD_TIME}\"};\n"
    "}\n")
]=])
add_custom_target(xscal_build_info ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XSCAL_BUILD_INFO_DIR}"
    COMMAND ${CMAKE_COMMAND}
        "-DOUTPUT=${XSCAL_BUILD_INFO_HEADER}"
        -P "${XSCAL_BUILD_INFO_SCRIPT}"
    BYPRODUCTS "${XSCAL_BUILD_INFO_HEADER}"
    VERBATIM)

add_library(dxgi SHARED
    ${XSCAL_CORE_SOURCES}
    src/plugins/root_keys.cpp
    src/plugins/revocations.cpp
    src/dllmain.cpp
    src/dxgi_proxy.cpp
    src/dxgi_proxy.asm
    src/dxgi_proxy.def
)

target_compile_features(dxgi PRIVATE cxx_std_20)
target_compile_definitions(dxgi PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _USE_STD_VECTOR_ALGORITHMS=0
    yyjson_inline=inline
    YYJSON_DISABLE_FAST_FP_CONV=1
    YYJSON_DISABLE_INCR_READER=1
    YYJSON_DISABLE_NON_STANDARD=1
    YYJSON_DISABLE_UTILS=1
    YYJSON_DISABLE_WRITER=1)
target_include_directories(dxgi PRIVATE
    src
    "${XSCAL_SDK_INCLUDE_DIR}"
    "${XSCAL_YYJSON_DIR}"
    "${XSCAL_LIBDEFLATE_DIR}"
    "${XSCAL_BUILD_INFO_DIR}")
target_link_libraries(dxgi PRIVATE
    version
    xinput
    winhttp
    crypt32
    comctl32)
target_compile_options(dxgi PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/EHsc;/GR->
    $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:/Gy;/Gw>)
target_link_options(dxgi PRIVATE /OPT:REF /OPT:ICF)
# Source options come after the target's, so /O2 replaces a size setting.
set_source_files_properties(${XSCAL_LIBDEFLATE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/O2;/w")
set_target_properties(dxgi PROPERTIES PREFIX "" OUTPUT_NAME "dxgi")
add_dependencies(dxgi xscal_build_info)
