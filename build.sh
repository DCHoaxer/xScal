#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
build_dir="${project_dir}/build/linux-xwin"
dist_dir="${project_dir}/dist"
xwin_root=${XWIN_ROOT:-"${HOME}/.xwin"}
xwin_cache="${HOME}/.xwin-cache"
sdk_version=10.0.26100
crt_version=14.44.17.14

sdk_include_dir=${XSCAL_SDK_INCLUDE_DIR:-}
yyjson_dir=${XSCAL_YYJSON_DIR:-}
libdeflate_dir=${XSCAL_LIBDEFLATE_DIR:-}
if [[ ! -f ${sdk_include_dir}/xscal/plugin_api.h ]]; then
    echo "Error: set XSCAL_SDK_INCLUDE_DIR to the directory containing xscal/plugin_api.h." >&2
    exit 1
fi
if [[ ! -f ${yyjson_dir}/yyjson.c ]]; then
    echo "Error: set XSCAL_YYJSON_DIR to the directory containing yyjson.c." >&2
    exit 1
fi
if [[ ! -f ${libdeflate_dir}/lib/deflate_decompress.c ]]; then
    echo "Error: set XSCAL_LIBDEFLATE_DIR to the libdeflate 1.22 source directory." >&2
    exit 1
fi

for program in cmake ninja clang-cl lld-link llvm-lib llvm-ml llvm-mt llvm-rc; do
    if ! command -v "${program}" >/dev/null 2>&1; then
        echo "Error: ${program} was not found on PATH." >&2
        echo "Install cmake, ninja, clang, lld, and llvm." >&2
        exit 1
    fi
done

if [[ ! -d ${xwin_root}/crt/include || ! -d ${xwin_root}/sdk/include ]]; then
    if ! command -v xwin >/dev/null 2>&1; then
        echo "Error: no Windows SDK at ${xwin_root} and xwin is not installed." >&2
        echo "Install it with: cargo install xwin --locked" >&2
        exit 1
    fi
    echo "Preparing the Windows SDK and CRT in ${xwin_root}..."
    xwin --accept-license \
        --cache-dir "${xwin_cache}" \
        --sdk-version "${sdk_version}" \
        --crt-version "${crt_version}" \
        --arch x86_64 \
        --variant desktop \
        splat --output "${xwin_root}"
fi

cmake -S "${project_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_TOOLCHAIN_FILE="${project_dir}/cmake/toolchains/xwin-clang.cmake" \
    -DXWIN_ROOT="${xwin_root}" \
    -DXSCAL_SDK_INCLUDE_DIR="${sdk_include_dir}" \
    -DXSCAL_YYJSON_DIR="${yyjson_dir}" \
    -DXSCAL_LIBDEFLATE_DIR="${libdeflate_dir}"
cmake --build "${build_dir}"

mkdir -p "${dist_dir}"
cp -f "${build_dir}/dxgi.dll" "${dist_dir}/dxgi.dll"
echo "Built ${build_dir}/dxgi.dll"
echo "Copied to ${dist_dir}/dxgi.dll"
