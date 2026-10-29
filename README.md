# xScal  

xScal enchances the game's Scaleform capabilities. 
Proof of concept. 

## Network access

xScal connects to the internet for two features, described below. Both can be
turned off in `xscal.ini`. xScal does not send telemetry or usage statistics,
does not download or run code, and does not read or change the game's own
network traffic.

### Address Library update check

**Purpose.** xScal needs a table of game addresses that matches the exact
Fallout 76 executable. Every game patch changes these addresses, and without a
matching table xScal stays inactive. The check fetches the table for a new
game build, or a corrected table for the current build, so users do not have to
wait for and install a manual download after each patch.

**When it connects.**

- At startup, when `Data\XSCAL\AddressLibrary\version-<game version>-<steam|gamepass>.bin`
  is missing or cannot be used (unsigned, damaged, or made for another build
  of the executable). This repeats on every launch until a usable file is
  installed.
- When a usable file is installed: at most once every 24 hours per game
  version, and on the first launch after xScal itself is updated.

**What it does.**

1. On a background thread, it sends one HTTPS GET request to
   `https://dcma.pl/addlib/v2/<file name>`, with 5-second timeouts. The file
   name contains only the game version and store, for example
   `version-1-7-27-11-steam.bin`. Nothing else is sent. As with any web
   request, the server sees the IP address and the requested file name.
2. It checks the response in memory before showing anything: a valid xScal
   signature, the game version, the store, the executable timestamp and image
   size, and the record format. A file that fails a check is discarded and the
   reason is written to the xScal log. The file holds only address and constant
   records; it cannot contain code.
3. It asks before installing. A Yes/No dialog names the game version and, for
   an update, the installed and new file revisions. A file is offered only when
   it is newer than the installed one.
   - **Yes** writes the file to `Data\XSCAL\AddressLibrary`. It is used after
     Fallout 76 is restarted.
   - **No** writes nothing. For an update, the declined file is recorded in a
     `.declined` file and is not offered again; a later, newer file is offered.
4. It records the time of the check in a `.checked` file next to the database.

If the server has no file or cannot be reached, xScal logs it and continues
with the installed file. When no usable file is installed, it shows an error
dialog with the Nexus link below.

**Turning it off.** Set `autoUpdate=false` in the `[AddressLibrary]` section.
xScal then makes no request. Install the files yourself from
https://www.nexusmods.com/fallout76/mods/4298/.

Code: `DownloadAddressLibrary` and `Library::Parse` in
[`src/address/address_library.cpp`](src/address/address_library.cpp),
`OfferAddressLibraryDownload` and `RunBackgroundJob` in
[`src/runtime/runtime.cpp`](src/runtime/runtime.cpp).

### Chat relay for Fallout Chat Mod

**Purpose.** xScal provides the native connection for Fallout Chat Mod's
in-game chat. Chat messages travel between players through a relay server, so
the feature cannot work without a connection.

**When it connects.** Only after Fallout Chat Mod's interface asks xScal to
connect. Without Fallout Chat Mod, xScal opens no chat connection. Once
connected, it reconnects after a dropped connection, waiting 250 ms at first
and up to 5 seconds between tries, until Fallout Chat Mod disconnects or the
game exits.

**What it sends.** Over a TLS WebSocket to `wss://falloutchatmod.com/relay`:
the display name and client version that Fallout Chat Mod provides, the
session token issued by the relay, and the messages, reports, and moderation
actions the player sends. Nothing else is sent.

**What it stores.** The session token, in `Data\XSCAL\chat-auth.bin`,
encrypted with Windows DPAPI for the current Windows user. Logging out deletes
the file.

**Turning it off.** Set `enabled=false` in the `[Chat]` section.
`relayEndpoint` changes the server; only `wss://` addresses are accepted.

Code: [`src/chat/`](src/chat/), with the connection in
[`src/chat/relay_connection.cpp`](src/chat/relay_connection.cpp) and the
exact messages in [`src/chat/relay_protocol.cpp`](src/chat/relay_protocol.cpp).

## Source layout

- `src/address`: Address library loading
- `src/api`: AS3 callbacks, movie interface, mod storage and file commands
- `src/chat`: Chat session and relay connection
- `src/common`: Small shared helpers (JSON, ASCII case)
- `src/config`: Config, EXE profiles and updateable RVAs
- `src/datamanager`: Pip-Boy effect and time data providers
- `src/hook`: Vtable stuf MovieRoot routing, and other stuff... and things
- `src/input`: XInput hook, HID gamepad reader, input state and gamepad suppression
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
- [libdeflate](https://github.com/ebiggers/libdeflate) 1.22 source (the directory containing `libdeflate.h` and `lib/`) — not included in this repo. xScal uses it to decompress `.ba2` archive entries.

On the first build, run these from an x64 Native Tools / Developer Command Prompt. Doing so tells CMake where the SDK, yyjson and libdeflate are:

```bat
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DXSCAL_SDK_INCLUDE_DIR=C:\path\to\xscal-sdk\include ^
  -DXSCAL_YYJSON_DIR=C:\path\to\yyjson\src ^
  -DXSCAL_LIBDEFLATE_DIR=C:\path\to\libdeflate
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
- The same xScal plugin SDK headers, yyjson source and libdeflate source as on Windows

Build:

```sh
XSCAL_SDK_INCLUDE_DIR=/path/to/xscal-sdk/include \
XSCAL_YYJSON_DIR=/path/to/yyjson/src \
XSCAL_LIBDEFLATE_DIR=/path/to/libdeflate \
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
