# xScal  

xScal enchances the game's Scaleform capabilities. 
Proof of concept. 

## Source layout

- `src/address`: Address library loading
- `src/api`: AS3 callbacks, movie interface, mod storage and file commands
- `src/chat`: Chat session and relay connection
- `src/common`: Small shared helpers (JSON, ASCII case)
- `src/config`: Config, EXE profiles and updateable RVAs
- `src/datamanager`: Pip-Boy effect and time data providers
- `src/hook`: Vtable stuf MovieRoot routing, and other stuff... and things
- `src/input`: XInput hook, input state and gamepad suppression
- `src/modules`: Built-in core and priority modules
- `src/platform`: Windows memory validation and diagnostics (i'm not even using windows)
- `src/plugins`: Plugin loading and signature checks
- `src/runtime`: Process initialization and shutdown, mostly.
- `src/scaleform`: Handler, values, and bridge attachment

## ASM??
Yes, this extenders implements runtime forwarder. 

## Build

Requirements (Windows x64 only):

- Visual Studio 2019 or newer (or Build Tools) with the C++ workload and a Windows 10/11 SDK
- CMake 3.20+ on `PATH`
- xScal plugin SDK headers (the directory containing `xscal/plugin_api.h`) — not included in this repo
- [yyjson](https://github.com/ibireme/yyjson) source (the directory containing `yyjson.c` and `yyjson.h`) — not included in this repo

On the first build, run these from an x64 Native Tools / Developer Command Prompt. Doing so tells CMake where the SDK and yyjson are:

```bat
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DXSCAL_SDK_INCLUDE_DIR=C:\path\to\xscal-sdk\include ^
  -DXSCAL_YYJSON_DIR=C:\path\to\yyjson\src
build.bat
```

For later builds, just run:

```bat
build.bat
```

`build.bat` locates Visual Studio via `vswhere` when not already in a developer prompt. The output is `build\dxgi.dll`, and it is also copied to `dist\dxgi.dll`. Delete the `build` folder to reset the configured paths.

### Linux (cross-compile)

The Windows DLL can be cross-compiled with clang-cl and the MSVC CRT and Windows SDK fetched by [xwin](https://github.com/Jake-Shadle/xwin).

Requirements:

- CMake 3.29+, Ninja, and clang, lld, and llvm (`clang-cl`, `lld-link`, `llvm-lib`, `llvm-ml`, `llvm-mt`, `llvm-rc`)
  - Fedora: `sudo dnf install cmake ninja-build clang lld llvm`
- xwin (`cargo install xwin --locked`), only if there is no Windows SDK in `~/.xwin` yet
- The same xScal plugin SDK headers and yyjson source as on Windows

Build:

```sh
XSCAL_SDK_INCLUDE_DIR=/path/to/xscal-sdk/include \
XSCAL_YYJSON_DIR=/path/to/yyjson/src \
./build.sh
```

The first run downloads the Windows SDK and CRT into `~/.xwin` (accepting Microsoft's license). Set `XWIN_ROOT` to use an existing xwin splat elsewhere. The output is `build/linux-xwin/dxgi.dll`, and it is also copied to `dist/dxgi.dll`.

## Extending callbacks

ActionScript callbacks are defined in [`src/api/scaleform_callbacks.cpp`](src/api/scaleform_callbacks.cpp).  

# DISCLAIMER

```text
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT.

IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT, OR OTHERWISE, ARISING FROM, OUT OF, OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
