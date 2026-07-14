# Subsplash Sync — OBS Studio Plugin

A native OBS Studio plugin that automatically starts and stops streaming based on
the broadcast schedule set in the Subsplash Dashboard.

## How It Works

```
┌───────────────────────┐              ┌──────────────────────────┐
│   Subsplash Live API  │◄── poll ────│  OBS Plugin               │
│                       │              │                          │
│  /tokens/v1/token     │              │  ┌─────────────────────┐ │
│  /live/v1/broadcasts  │── schedule ─►│  │ Background Thread   │ │
│                       │              │  │ (scheduler.c)       │ │
└───────────────────────┘              │  └────────┬────────────┘ │
                                       │           │ action flag  │
                                       │  ┌────────▼────────────┐ │
                                       │  │ Main Thread Timer   │ │
                                       │  │ streaming_start()   │ │
                                       │  │ streaming_stop()    │ │
                                       │  └─────────────────────┘ │
                                       └──────────────────────────┘
```

1. A background thread polls the Subsplash Live API for upcoming broadcasts.
2. When a broadcast's scheduled `start_at` approaches, it signals the main thread.
3. The main thread calls `obs_frontend_streaming_start()` to begin streaming.
4. When the broadcast's `end_at` time passes, it calls `obs_frontend_streaming_stop()`.

The user configures their OBS Stream settings (RTMP URL + stream key from their
Subsplash Live account) separately. This plugin only controls **when** streaming
starts and stops.

## Prerequisites

- OBS Studio 31+
- A Subsplash Live account
- A Subsplash API Client (client_id / client_secret)
- OBS Stream settings configured with your Subsplash streaming URL and stream key

## Installing

### Windows

- Download `obs-subsplash-sync-x.y.z-windows-x64-Installer.exe` from the Releases page
- Run it on your machine and follow the installer steps
- If OBS is already running, restart it so it can detect the plugin

### Mac OS

- Download `obs-subsplash-sync-x.y.z-macos-universal.pkg` from the Releases page
- Run it on your machine and follow the installer steps
- If OBS is already running, restart it so it can detect the plugin

### Linux (Ubuntu, Debian)

- Download `obs-subsplash-sync-x.y.z-x86_64-linux-gnu.deb` from the Releases page
- Run it on your machine and follow the installer steps
- If OBS is already running, restart it so it can detect the plugin

### Linux (other)

- Download the tarball `obs-subsplash-sync-x.y.z-x86_64-linux-gnu.tar.xz` from the Releases page
- Locate the plugins directory for your OBS installation and copy the tarball's contents into it
- If OBS is already running, restart it so it can detect the plugin

## Usage

1. Launch OBS Studio.
2. Go to **Docks > Subsplash Sync**.
3. Enter your Subsplash API credentials:
   - **Client ID** and **Client Secret** from your API Client
   - **App Key** (6-character code for your organization)
4. Click **Test Connection** to verify credentials and see upcoming broadcasts.
5. Adjust schedule settings:
   - **Start Lead**: Minutes before scheduled start to begin streaming (default: 2)
   - **Stop Lag**: Minutes after scheduled end to stop streaming (default: 2)
6. Click **Enable Sync**.

The plugin can be disabled under OBS's Plugin Manager (**Tools > Plugin Manager**).

The plugin persists your settings and will auto-start the scheduler when OBS
launches if it was previously enabled.

Your API credentials (Client ID, Client Secret, and App Key) are stored in the
OS-native credential store (macOS Keychain, Windows Credential Manager, or your Linux distribution's configured keychain manager) via QtKeychain — never in plaintext on disk. Non-secret settings remain in the plugin's `config.json`.

## Broadcast Lifecycle

Broadcasts follow this status flow:

```
scheduled → live → ended → on-demand
                        ↘ never-happened
```

The plugin starts streaming when a broadcast is `scheduled` and its start time
arrives. It stops streaming when the `end_at` time passes.

Simulated-live broadcasts are do **not** trigger streaming, only true live broadcasts.

## Limitations and known issues

- Manages a single stream (one broadcast at a time)
- Transient API errors are retried with jittered exponential backoff (2s up to 60s); failures fall back to the cached broadcast end time so a scheduled STOP still fires
- Currently we only provide a locale file for English UI strings
- If a broadcast event is deleted while a stream is running, streaming will **not** stop automatically; the user will need to stop it manually

## Building

This plugin uses the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
build system. Follow the standard OBS plugin build workflow:

### Windows

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

### Mac OS

```bash
cmake --preset macos
cmake --build --preset macos
```

### Linux

```bash
cmake --preset linux-x86_64
cmake --build --preset linux-x86_64
```

See the [OBS Plugin Template Wiki](https://github.com/obsproject/obs-plugintemplate/wiki)
for detailed build instructions and prerequisites.

### Running Tests

Unit tests live in the `tests/` directory and can be built standalone
without the OBS SDK. CMocka and cJSON are fetched automatically via
CMake `FetchContent`.

```bash
cd tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

### Dependencies

- **libobs** and **obs-frontend-api** — provided by the OBS SDK
- **Qt6** (Widgets, Core) — provided by the OBS SDK build system
- **libcurl** — system library (macOS/Linux) or bundled (Windows)

## Installation

After building, copy the plugin binary to your OBS plugins directory:

- **macOS**: `~/Library/Application Support/obs-studio/plugins/`
- **Linux**: `~/.config/obs-studio/plugins/`
- **Windows**: `%APPDATA%\obs-studio\plugins\`

## Architecture

| File | Language | Role |
|---|---|---|
| `src/plugin-main.cpp` | C++ | Module init, tools menu, config persistence, action timer |
| `src/subsplash-api.c/h` | C | libcurl HTTP client for Subsplash auth + broadcast API |
| `src/scheduler.c/h` | C | Background poll thread with atomic action signaling |
| `src/scheduler-panel.cpp/hpp` | C++ | Qt dock panel for credentials, settings, and status |
| `src/credential-store.cpp/hpp` | C++ | Secure credential storage via QtKeychain (OS keychain) |
| `tests/` | C | Unit tests (CMocka) for scheduler and API logic |

### Thread Safety

- The **background thread** polls the API and sets an atomic action flag.
- The **main thread** (Qt event loop) drains the flag via a 1-second QTimer
  and calls `obs_frontend_streaming_start/stop()`.
- Token caching is protected by a `pthread_mutex_t`.
- Status strings are copied under a separate mutex.

### Key Subsplash APIs

| Endpoint | Method | Purpose |
|---|---|---|
| `/tokens/v1/token` | POST | Authenticate (client_credentials grant) |
| `/live/v1/broadcasts` | GET | List upcoming/scheduled broadcasts |
