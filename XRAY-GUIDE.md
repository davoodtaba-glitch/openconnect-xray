# OpenConnect VPN + Xray/V2Ray — Guide

This document describes the fork in this workspace: **OpenConnect GUI 1.6.2** with
native **Xray/V2Ray profile support** and a bundled **Xray-core 26.10.31**.

Date: 2026-11-02  
Base project: `openconnect/openconnect-gui` (GitLab, `main`, commit `f210cb5`)  
Platform for Xray support: **Windows 10/11 (x64)**

---

## 1. Overview

The application keeps its original OpenConnect SSL-VPN functionality and adds a
second profile family powered by the bundled Xray core.

Two connection modes are available per Xray profile:

| Mode | Behavior | Requirements |
|---|---|---|
| **TUN (global VPN)** | Routes all IPv4 traffic through Xray using a Wintun adapter (`oc-xray`, `198.18.0.1/16`). | Administrator rights (the exe manifest requests UAC). |
| **System proxy** | Runs a local mixed HTTP/SOCKS inbound and sets the Windows proxy to `127.0.0.1:<random port>`. | Only affects applications that honor the Windows/WinINet proxy. |

Xray is started as a child process. A Windows Job Object is used so the core is
terminated if the GUI exits unexpectedly. System-proxy settings and TUN adapter
DNS are restored on a clean disconnect; a stale proxy backup is restored on the
next application start.

---

## 2. Feature summary

- Import full V2Ray/Xray client JSON, share links, subscriptions and subscription URLs.
- Share links: `vmess://`, `vless://` (including REALITY), `trojan://`, `ss://`,
  `socks://`, `socks5://`, `http://`.
- Multi-link imports create multiple profiles.
- Subscription URL is stored per profile for fast updates.
- Subscription updater updates profiles in place, adds only new nodes, and can
  remove nodes no longer present.
- Per-profile advanced options:
  - Server / port override
  - DNS servers, applied to the tunnel and the active physical adapter
  - DNS leak protection with automatic restore
  - Domain sniffing
  - QUIC blocking (UDP/443) to avoid HTTP/3 errors
  - TLS fragment (`finalmask.tcp`), including packet type, length and delay
- Live upload/download **speed** plus cumulative totals for both OpenConnect and Xray profiles.
- Live traffic counters (Xray StatsService).
- Xray stdout/stderr forwarded to **View log**.
- Edit, remove selected, and remove all profiles.
- Bundled `xray.exe`, `wintun.dll`, `geoip.dat`, `geosite.dat` and licenses.

---

## 3. Quick start

1. Either run the portable folder:
   ```
   G:\grok\open connect\dist\OpenConnect-Xray\openconnect-gui.exe
   ```
   or install it with:
   ```
   G:\grok\open connect\dist\OpenConnect-Xray-Setup-1.6.2.exe
   ```
2. Accept the UAC prompt.
3. Open **File → Profiles → New Xray / V2Ray Profile...**
4. Paste a config, share link or subscription URL.
5. Choose **TUN** or **System proxy**.
6. Press **Save**.
7. Select the profile and press **Connect**.

> If an old instance is already running (including minimized to the tray), the
> app is single-instance: launching a new exe only activates the old window.
> Use **File → Quit** or the tray menu → **Quit** first.

The window is freely resizable (floor 320×220), both tabs scroll when needed,
and the last window size/position is restored on the next start.

---

## 4. Creating Xray profiles

### 4.1 Accepted input

Paste one of the following into the config box:

1. **Full JSON config** (client-side, must contain `outbounds`):
   ```json
   {
     "outbounds": [
       {
         "tag": "proxy",
         "protocol": "vless",
         "settings": { "vnext": [ ... ] },
         "streamSettings": { ... }
       }
     ]
   }
   ```
   A single outbound object is also accepted and wrapped into `outbounds`.

2. **Share links**, one per line:
   ```
   vless://uuid@server:443?security=reality&sni=example.com&pbk=...&sid=...&type=tcp#Node
   vmess://BASE64
   trojan://password@server:443?security=tls&type=ws&path=/ws#Node
   ss://BASE64(method:password)@server:8388#Node
   ```

3. **Subscription URL**:
   - Put it in the URL field and press **Fetch / Update**, **or**
   - Paste the URL in the config box and press Save (it fetches first).

4. **Base64 subscription content** pasted directly.

5. **File** via **Load file...** (`.json`, `.txt`, `.conf`).

A **server-side** config containing only `inbounds` is rejected: a client
config or share link is required.

### 4.2 Naming

- The name comes from the link remark (`#name`) or the server host.
- Duplicate names receive ` (2)`, ` (3)`, etc.
- The optional **Profile name** field overrides the imported name.
- When a subscription produces multiple profiles, each is saved separately.

### 4.3 Supported / unsupported

Supported: VMess, VLESS (TCP/WS/gRPC/HTTPupgrade/HTTP2/KCP/QUIC/REALITY),
Trojan, Shadowsocks, SOCKS, HTTP proxy.

Not supported: Shadowsocks SIP003 plugins, Hysteria, TUIC, non-Xray protocols.

---

## 5. Editing profiles and advanced options

Open **File → Profiles → Edit selected profile** (`Ctrl+E`).

### 5.1 Basic fields

| Field | Meaning |
|---|---|
| **Profile name** | Display name and settings key. |
| **Tunnel mode** | TUN (global VPN) or System proxy. |
| **Config box** | Current JSON. Can be replaced by a new config/link. |
| **Subscription URL** | Saved URL used by **Fetch / Update**. |

### 5.2 Advanced fields

| Field | Default | Behavior |
|---|---|---|
| **Server override** | empty | Replaces the primary outbound’s server address (`vnext`/`servers`). |
| **Port** | `0` = from config | Overrides the primary outbound’s port. |
| **DNS servers** | empty = `1.1.1.1, 1.0.0.1` | Comma/space-separated **IPv4** addresses. |
| **Sniffing** | off | Enables HTTP/TLS/QUIC domain sniffing on the active inbound. |
| **Block QUIC** | on | Drops UDP/443 so browsers fall back to TCP when QUIC is unsupported or broken by the server. |
| **Fragment** | off | Enables TLS-handshake fragmentation. |
| **Type** | `tlshello` | `tlshello`, `all packets`, `1-1 … 1-5`, or a custom range like `1-8`. |
| **Length** | `100-200` | Fragment size range in bytes. |
| **Delay (ms)** | `10-20` | Delay range between fragments. |

Notes:

- Server/port overrides apply only to the first outbound that has a
  `vnext` or `servers` list. They are not applied to `freedom`.
- Sniffing:
  - TUN: `routeOnly: true`, so routing sees the domain while the connection keeps
    its IP.
  - System proxy: mixed inbound with HTTP/TLS/QUIC sniffing.
- **Block QUIC** adds a blackhole route for `network: udp, port: 443`. This is the
  recommended fix for `ERR_QUIC_PROTOCOL_ERROR` on sites such as YouTube and
  Instagram when the proxy/server does not carry UDP properly. Disable it only if
  your server fully supports QUIC/HTTP3. In system-proxy mode, an application
  that ignores the proxy can still attempt QUIC directly; use TUN mode for
  complete control.
- Fragmentation is written as:
  ```json
  "streamSettings": {
    "finalmask": {
      "tcp": [{
        "type": "fragment",
        "settings": {
          "packets": "tlshello",
          "length": "100-200",
          "delay": "10-20"
        }
      }]
    }
  }
  ```
  It applies to TCP-based transports of the primary proxy outbound.
- Unchanged prefilled server/port fields do not freeze the endpoint. Only an
  explicitly entered/edited override is stored.
- Paste/Load replace the config and clear the saved subscription URL.

---

## 6. Subscription management

### 6.1 URL persistence

- Every profile created from a subscription stores `xray-subscription-url`.
- On edit, the URL is prefilled automatically.
- **Fetch / Update** reloads the subscription.
- If a subscription returns several nodes while editing one profile, Save asks
  which node should update this profile.

### 6.2 Update all profiles (no duplicates)

Use **File → Profiles → Update Xray Subscription...** (also in the server
dropdown menu).

Flow:

1. It lists all saved subscription URLs; choose one if there are several.
2. It fetches and parses the subscription.
3. Each node is matched to an existing profile:
   - first by **profile name** (case-insensitive),
   - then by **server endpoint** (`host:port`).
4. Matched profiles are updated in place. Their name, mode, DNS, sniffing,
   fragmentation and explicit server/port overrides are preserved.
5. Unmatched nodes are added as new profiles. They inherit mode and advanced
   settings from the first profile in that subscription group.
6. Profiles no longer present in the subscription are listed, with a prompt to
   remove them.
7. A summary shows: updated / added / removed / kept.

Caveat: if a provider changes **both** a node’s name and its server address, it
cannot be recognized as the same node. It appears as a new node, and the old one
appears in the stale list for removal.

Profiles imported before subscription-URL persistence must be saved once with
their URL:

`Edit → paste URL in the subscription field → Fetch / Update → Save`

---

## 7. Removing profiles

- **Remove selected profile** (`Ctrl+R`) removes one profile.
- **Remove All Profiles...** removes every OpenConnect and Xray profile.
  - Shows the count and requires explicit confirmation.
  - Refuses to run while a connection is active.
  - Deletes only local profile settings; nothing is changed on servers.
  - Cannot be undone (back up the registry key first if needed).

Backup example:

```bat
reg export "HKCU\Software\OpenConnect-GUI Team\OpenConnect-GUI" profiles-backup.reg
```

---

## 8. How it works internally

### 8.1 Runtime config generation

The stored profile JSON is copied and extended at connect time:

- Outbounds, user DNS and routing rules are preserved.
- `log` is set to warning/no access log.
- `stats`, `api` (StatsService) and `policy.system` counters are added.
- `inbounds` are replaced with:
  - API inbound on loopback (random port).
  - TUN inbound (`oc-xray`, `198.18.0.1/16`, DNS, default route) **or**
    mixed HTTP/SOCKS inbound on loopback (random port).
- Routing gets a rule sending API traffic to the API outbound.
- If **Block QUIC** is on, a blackhole outbound/rule drops UDP port 443.
- Sniffing, endpoint override and fragment settings are applied if enabled.

The generated file is written to:

```
%APPDATA%\OpenConnect-GUI Team\OpenConnect VPN\xray-runtime.json
```

and removed on disconnect.

### 8.2 TUN readiness and DNS

- The app waits until the adapter actually has `198.18.0.1` before reporting
  **Connected**.
- Adapter name is detected dynamically by its address (normally `oc-xray`).
- DNS is assigned both through Xray’s TUN settings and `netsh` on the detected
  adapter, then `ipconfig /flushdns` runs.
- **DNS leak protection:** the app selects the main physical adapter (an active
  adapter with an IPv4 address/default gateway and the lowest metric, excluding
  TAP/Wintun/VPN/Tunnel adapters), saves its original IPv4 DNS source, and
  temporarily points it at the profile’s DNS servers while TUN is connected.
- On disconnect, the original physical-adapter DNS is restored (static list or
  DHCP), the TUN adapter DNS is reset, and caches are flushed.
- If the app is force-killed, the saved physical DNS backup is restored on the
  next app start before any connection is made.
- `ipconfig /all` and `netsh interface ipv4 show addresses` remain authoritative;
  the Windows adapter-properties dialog can show stale values.
- **Windows adapter properties can show stale/blank values**; `ipconfig /all`
  and `netsh interface ipv4 show addresses` are authoritative.

### 8.3 System proxy mode

- A mixed inbound listens on `127.0.0.1:<random>`.
- The previous Windows proxy state is saved in app settings.
- `ProxyEnable` / `ProxyServer` are set and WinINet is refreshed.
- On disconnect the previous values are restored.
- If the app was force-killed, the backup is restored on the next app start.

### 8.4 Process management and statistics

- `xray.exe run -c <runtime.json>` is started with `XRAY_LOCATION_ASSET` pointing
  to the app directory.
- A job object is created with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
- Readiness polls the API port (and, for TUN, the adapter address).
- Counters are read from:
  ```
  xray api statsquery --server=127.0.0.1:<apiPort> -pattern "inbound>>>oc-in>>>traffic>>>"
  ```
- Stats are polled every 2 seconds. The Download/Upload rows show
  `live speed · cumulative total`, e.g. `1.24 MB/s · 38.5 MB`.
  Speed is computed from counter deltas and resets on reconnect.
- Xray errors, warnings and info lines go to **View log**.

---

## 9. Building from source (Windows)

### 9.1 Requirements

- **Qt 6.8.3 MinGW 64-bit** (`win64_mingw`) plus the **qtscxml** module
  (provides Qt6StateMachine).
- **GCC 13.1** MinGW toolchain matching Qt.
- CMake 3.28+, Ninja, Git.
- OpenConnect v8.10 prebuilt MinGW packages placed in `external/`.
- Internet access on first configure (spdlog and qt-solutions are downloaded).

Example Qt install with `aqt`:

```bat
python -m aqt install-qt windows desktop 6.8.3 win64_mingw -m qtscxml --outputdir G:\Qt
python -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 --outputdir G:\Qt
```

OpenConnect packages (rename to the lowercase `_mingw64` form expected by CMake):

```
openconnect-devel-v8.10_MINGW64.zip  -> openconnect-devel-v8.10_mingw64.zip
openconnect-v8.10_MINGW64.zip        -> openconnect-v8.10_mingw64.zip
(+ their .sha512 files)
```

Download from: `https://github.com/horar/openconnect/releases/tag/v8.10`

Xray-core is vendored in `third_party/xray/`. If `xray.exe` is missing, CMake
downloads `Xray-windows-64.zip` v26.10.31 with a pinned SHA-256.

### 9.2 Configure

```bat
cd /d "G:\grok\open connect"
set PATH=G:\Qt\Tools\mingw1310_64\bin;G:\Qt\6.8.3\mingw_64\bin;%PATH%

cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -Dopenconnect-TAG=v8.10 ^
  -DCMAKE_PREFIX_PATH=G:/Qt/6.8.3/mingw_64 ^
  -DCMAKE_C_COMPILER=G:/Qt/Tools/mingw1310_64/bin/gcc.exe ^
  -DCMAKE_CXX_COMPILER=G:/Qt/Tools/mingw1310_64/bin/g++.exe ^
  -DCMAKE_RC_COMPILER=G:/Qt/Tools/mingw1310_64/bin/windres.exe ^
  "-DCMAKE_ARGS=-DCMAKE_C_COMPILER=G:/Qt/Tools/mingw1310_64/bin/gcc.exe;-DCMAKE_CXX_COMPILER=G:/Qt/Tools/mingw1310_64/bin/g++.exe;-DCMAKE_RC_COMPILER=G:/Qt/Tools/mingw1310_64/bin/windres.exe"
```

`CMAKE_ARGS` is required so the child CMake projects (spdlog, qt-solutions) use
MinGW instead of MSVC.

### 9.3 Pre-extract OpenConnect libraries

Ninja resolves imported library files at configure time, so extract them before
building:

```bat
mkdir build\external\lib build\external\include
:: extract devel zip: copy include\* and lib\* into build\external
:: extract runtime zip: copy all DLLs / .dll.a into build\external\lib
```

### 9.4 Build

```bat
cmake --build build -j 8
```

Output: `build\bin\openconnect-gui.exe`.

Post-build steps automatically copy:

- Xray core, Wintun, geo data and licenses
- Qt runtime via `windeployqt`
- Matching GCC 13 `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`
- Application and third-party licenses

Close any running instance before rebuilding; Windows locks the exe.

### 9.5 Packaging

The ready-to-run folder is `dist\OpenConnect-Xray`. To recreate it, copy
`build\bin\*` there; Qt deployment is included by the build.

Build notes:

- Paths containing spaces are supported (the RC resource is compiled from the
  binary directory).
- The repository must have full git history/tags; a shallow clone makes the
  `git describe` step fail (`git fetch --unshallow`).
- `CMake/Includes/ProjectExternals_qt-solutions.cmake` was fixed for newer CMake.
- OpenConnect v8.10 is used because no official v9.12 MinGW packages are
  published. `src/vpninfo.cpp` has API guards so v8.10 builds; the “disable UDP”
  option logs a limitation on API 5.8.

### 9.6 Windows installer

Script: `installer\OpenConnect-Xray.nsi`  
Output: `dist\OpenConnect-Xray-Setup-1.6.2.exe`

Build with NSIS 3:

```bat
"C:\Program Files (x86)\NSIS\makensis.exe" "G:\grok\open connect\installer\OpenConnect-Xray.nsi"
```

Use an absolute script path so `${__FILEDIR__}` resolves correctly.

The installer:

- Installs to `%ProgramFiles%\OpenConnect Xray` (per-machine, UAC required).
- Includes the portable folder contents, guide and licenses.
- Creates Start Menu entries plus an optional desktop shortcut.
- Offers a **TAP-Windows driver** component (`devcon` from the signed 9.27.0
  package) for OpenConnect profiles. Xray TUN does not need it (Wintun is a DLL).
- Refuses to continue while the app is running.
- Registers a standard uninstaller in Apps & Features.
- On uninstall asks whether to delete saved profiles; default is **No**.
  The TAP driver is left installed because other VPN clients may share it.

A silent test install/uninstall into a temporary directory is supported with
`/S /D=<path>`; the release installer itself requests administrator rights.

---

## 10. Profile storage keys

Windows registry:

```
HKCU\Software\OpenConnect-GUI Team\OpenConnect-GUI
```

Per profile, under `server:<name>/`:

| Key | Purpose |
|---|---|
| `server` | Display gateway/endpoint |
| `xray-config` | Client JSON (plain text) |
| `xray-mode` | `tun` or `proxy` |
| `xray-subscription-url` | Saved subscription URL |
| `xray-dns` | Comma-separated IPv4 DNS servers |
| `xray-sniffing` | bool |
| `xray-block-quic` | bool (default true) |
| `xray-fragment` | bool |
| `xray-fragment-packets` | `tlshello`, empty (all), or range |
| `xray-fragment-length` | e.g. `100-200` |
| `xray-fragment-interval` | e.g. `10-20` |
| `xray-server-override` | optional host |
| `xray-port-override` | 0 = from config |

OpenConnect profiles continue to use the original keys
(`batch`, `username`, certificates, `protocol-name`, etc.).

Profiles can contain UUIDs/passwords and subscription tokens in plain text.
Protect the Windows account and registry.

---

## 11. Verification performed

Completed:

- Clean Release build with Qt 6.8.3 / GCC 13.1.
- Parser tests:
  - VMess (including numeric port/alterId)
  - VLESS + REALITY
  - Trojan
  - Shadowsocks
  - SOCKS
  - multiple links and base64 subscriptions
  - name sanitization and duplicate suffixes
- Generated configs validated with the real core:
  ```
  xray run -test -c <config>
  ```
  including TUN, system proxy, DNS lists, sniffing on/off, server/port
  overrides, and all fragment packet types (`tlshello`, all, `1-3`).
- Live HTTP test through the generated proxy returned `HTTP 200`.
- Stats counters read successfully from the StatsService API.
- Windows adapter verified with `ipconfig /all` and
  `netsh interface ipv4 show addresses name=oc-xray`:
  `198.18.0.1/16`, DNS `1.1.1.1` / `1.0.0.1`.

Not automated: interactive GUI flows. The UI is compiled and packaged, but
buttons/dialog layout should be exercised manually.

---

## 12. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| App does not start, error `0xC0000139` | Mismatched MinGW runtime. Use the packaged `dist` folder or rebuild; the build deploys GCC 13 runtime DLLs. |
| Missing `Qt6Core.dll` etc. | Run the exe from `dist\OpenConnect-Xray`, or run `windeployqt` on the exe. |
| Rebuild fails: “Permission denied” linking exe | The app is running. Quit it (including tray) and rebuild. |
| New menu items missing | An older instance is still running (single-instance mode). Quit via tray/File → Quit, then start the new exe. |
| `invalid "spiderX"` from Xray | The app normalizes REALITY `spiderX` (decode, trim, add `/`). If it persists, the public key/short ID is invalid. |
| TUN adapter properties show blank IP | The properties dialog caches old state. Check `ipconfig /all` or `netsh interface ipv4 show addresses name=oc-xray`; the app waits for `198.18.0.1` before showing Connected. |
| TUN has no internet | Check `route print 0.0.0.0` for the default route via `oc-xray`; confirm the server supports UDP (needed for DNS); test HTTP rather than `ping` (ICMP is not tunneled). |
| DNS not applied | DNS must be IPv4. Check View log for `netsh` warnings and run `ipconfig /all`. |
| Physical adapter DNS still shows old servers while connected | Leak protection skips virtual adapters and requires a physical adapter with an IPv4 address. Check View log for `DNS leak protection` messages. |
| DNS broken after force-killing the app | The saved backup is restored on the next app start. Restart the app once, or set the adapter DNS back manually. |
| System proxy mode does not cover an app | The app ignores the Windows proxy. Use TUN mode. |
| `ERR_QUIC_PROTOCOL_ERROR` (YouTube/Instagram) | HTTP/3 is failing through the tunnel. Keep **Block QUIC (UDP/443)** enabled in the profile; reconnect. In system-proxy mode also use TUN if the app bypasses the proxy. |
| Subscription update creates a new profile instead of updating | Provider changed both name and endpoint; remove the stale entry when prompted. |
| Subscription update action says no URL | Profile was imported before URL persistence. Edit it, paste the URL, Fetch/Update, Save. |
| Subscription fetch fails | Provider may block the app’s User-Agent/region. Paste the node links directly. |
| Fragment has no effect | Fragment applies to the primary TCP proxy outbound; UDP/QUIC transports and other outbounds are not affected. |
| Xray exits with code 23 | Configuration rejected. Open View log; frequent causes are invalid REALITY keys or unsupported outbound settings. |
| Removing profiles while connected | Disconnect first; both single and bulk removal are blocked during an active connection. |

---

## 13. Security and limitations

- **IPv4 DNS leak protection is active in TUN mode** for the main physical
  adapter. It does not cover IPv6 DNS, secondary physical adapters, applications
  using their own DNS-over-HTTPS/TLS, or a separate active VPN adapter’s DNS.
  TUN still routes IPv4 only.
- There is no general kill switch: if Xray exits unexpectedly, traffic may fall
  back to the physical network until the app restores routes/DNS.
- System proxy mode does not tunnel DNS; applications that ignore the Windows
  proxy bypass it entirely.
- TUN mode requires administrator rights and modifies routes and adapter DNS.
- The app stores profile JSON (including credentials) and subscription URLs in
  plain text in the registry.
- Force-killing the app can leave the system proxy pointing at a dead local
  port; the next app start restores the saved proxy state.
- Bundled `xray.exe` is a third-party Go binary and may trigger antivirus
  heuristics. Verify the release hash if needed.
- Xray support is Windows-only in this build. macOS/Linux builds of the GUI are
  not wired to an Xray binary.
- OpenConnect links against v8.10; profiles using “disable UDP” cannot call the
  newer `openconnect_disable_dtls` API and only skip DTLS setup.

Licenses shipped in `dist\OpenConnect-Xray`:

- `LICENSE.txt` — GPLv2 (application)
- `LICENSE-Xray.txt` — Xray-core
- `LICENSE-Wintun.txt` — Wintun

---

## 14. Files added / modified

Added:

```
src/xray/XrayConfig.h/.cpp              Parsing, runtime config generation
src/xray/XrayClient.h/.cpp              Core process, TUN/DNS, proxy, stats
src/xray/XraySubscriptionUpdater.h/.cpp Subscription sync (no duplicates)
src/dialog/XrayProfileDialog.h/.cpp/.ui Xray profile editor
third_party/xray/                       xray.exe, wintun.dll, geo data, licenses
dist/OpenConnect-Xray/                  Runnable Windows build
```

Modified:

```
src/dialog/mainwindow.h/.cpp            Menus, connect/disconnect, update/remove-all
src/server_storage.h/.cpp               Xray profile fields
src/CMakeLists.txt                      Sources, Qt deploy, Xray bundle, RC build
src/vpninfo.cpp                         OpenConnect API 5.9 guards
CMake/Includes/ProjectExternals_qt-solutions.cmake  CMake fix
external/                               OpenConnect v8.10 prebuilt packages
```

---

## 15. Versions and sources

| Component | Version |
|---|---|
| OpenConnect GUI | 1.6.2 (`f210cb5` + local changes) |
| Xray-core | 26.10.31 |
| Wintun | 0.14 |
| Qt | 6.8.3 MinGW 64-bit |
| GCC | 13.1.0 |
| OpenConnect library | 8.10 (prebuilt MinGW) |
| CMake | 3.28.1 |
| Ninja | installed with Qt/conda |

Links:

- OpenConnect GUI: https://gitlab.com/openconnect/openconnect-gui
- Xray-core: https://github.com/XTLS/Xray-core
- OpenConnect MinGW packages: https://github.com/horar/openconnect/releases
- OpenConnect project: https://www.infradead.org/openconnect/
