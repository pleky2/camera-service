# Photobox Camera Service

## Overview
This is a **C++ REST API server** that controls a **Sony A7III (ILCE-7M3)** camera via USB using the **Sony Camera Remote Command (PTP/WIA protocol)**. It acts as a local microservice that can be consumed by any frontend UI (React, Vue, Electron, etc.) or payment gateway integration.

The Sony A7III is **not supported** by Sony's official Camera Remote SDK (CRSDK), so we use the alternative **Camera Control PTP** protocol instead, which communicates via Windows WIA (Windows Image Acquisition) + PTP vendor commands.

## Tech Stack
- **Language:** C++17
- **Build System:** CMake 3.10+
- **Compiler:** MSVC (Visual Studio 2022)
- **HTTP Server:** [cpp-httplib](https://github.com/yhirose/cpp-httplib) (header-only)
- **Camera Protocol:** Sony PTP via Windows WIA (`IWiaDevMgr`, `IWiaItemExtras::Escape`)
- **Platform:** Windows only (depends on WIA COM interfaces)

## Project Structure

```
photobox-camera-service/
├── CMakeLists.txt                  # Build configuration
├── PROJECT_CONTEXT.md              # This file
├── src/
│   └── main.cpp                    # HTTP Server + Camera API endpoints
├── include/
│   └── httplib.h                   # cpp-httplib (header-only HTTP server library)
└── sony_ptp/                       # Sony PTP core files (extracted from Sony's example)
    ├── PTPControl.cpp/.h           # Core PTP command sender (connect, capture, get object)
    ├── PTPDef.h                    # PTP protocol constants, operation codes, device property codes
    ├── DataManager.cpp/.h          # Camera state/property manager
    ├── DevicePropItemList.h        # Lookup tables for camera settings (ISO, Aperture, Shutter, Drive Mode, etc.)
    ├── resource.h                  # Stub resource IDs (replaces original MFC UI resource IDs)
    └── stdafx.h                    # Precompiled header stub
```

## How to Build

```powershell
# From the parent directory (example-v2-windows)
cmake -B build -S photobox-camera-service
cmake --build build --config Release

# Output: build/Release/CameraService.exe
```

## How to Run

```powershell
.\build\Release\CameraService.exe
```

The server starts on `http://localhost:8080`.

## API Reference

### `GET /` — Health Check
Returns server status and camera connection state.

**Response:**
```json
{
  "status": "ok",
  "message": "Camera Service Running",
  "camera_connected": false
}
```

### `GET /camera-info` — List Connected Cameras
Enumerates all WIA camera devices connected via USB.

**Response:**
```json
{
  "cameras": ["ILCE-7M3"],
  "connected": false
}
```

### `POST /connect` — Connect to Camera
Performs the full SDIO handshake sequence required by Sony PTP:
1. `prepareConnection()` — WIA device enumeration & connection
2. `SDIOConnect(1, ...)` — Phase 1
3. `SDIOConnect(2, ...)` — Phase 2
4. `SDIOGetExtDeviceInfo(0xC8)` — Get extension version
5. `SDIOConnect(3, ...)` — Phase 3

**Response:**
```json
{"status": "ok", "message": "Camera connected successfully"}
```

### `POST /capture` — Capture Photo (Normal Drive Mode)
Takes a single photo using the following sequence:
1. `SDIOControlDevice(DPC_S1_BUTTON, DOWN)` — Half-press shutter (autofocus)
2. `SDIOControlDevice(DPC_S2_BUTTON, DOWN)` — Full-press shutter (capture)
3. `SDIOControlDevice(DPC_S2_BUTTON, UP)` — Release
4. `SDIOControlDevice(DPC_S1_BUTTON, UP)` — Release
5. `GetObjectInfo(0xFFFFC001)` — Get image metadata
6. `ExecuteGetObject(0xFFFFC001)` — Download JPEG from camera
7. Save to disk

**Response:**
```json
{
  "status": "ok",
  "message": "Photo captured successfully",
  "file_name": "capture_20260518_192143_995.jpg",
  "file_path": "C:\\photobox\\captures\\capture_20260518_192143_995.jpg"
}
```

### `POST /set-save-path` — Set Save Directory
Changes where captured photos are saved. Creates directories recursively.

**Request Body:**
```json
{"path": "X:\\my-photos"}
```

**Response:**
```json
{"status": "ok", "save_path": "X:\\my-photos"}
```

### `GET /images/<filename>` — Serve Captured Images
Static file server for the save directory. Access captured photos via browser.

### `GET /liveview` — Live View Stream *(Not yet implemented)*
Will stream MJPEG from the camera's live view buffer.

## Sony PTP Protocol Details

### Connection Flow
The camera MUST be set to **PC Remote** mode (not Mass Storage) before connecting.

```
USB Cable → WIA Device Manager → IWiaItem → IWiaItemExtras::Escape()
                                                    ↓
                                         PTP Vendor Commands (Sony SDIO)
```

### Key PTP Operation Codes (from `PTPDef.h`)
| Code | Name | Purpose |
|------|------|---------|
| `0x9201` | `SDIOConnect` | Establish SDIO session (3 phases) |
| `0x9202` | `SDIOGetExtDeviceInfo` | Get supported commands |
| `0x9205` | `SDIOSetExtDevicePropValue` | Set camera property (ISO, aperture, etc.) |
| `0x9207` | `SDIOControlDevice` | Send control commands (shutter, AE lock, etc.) |
| `0x9209` | `SDIOGetAllExtDeviceInfo` | Poll all camera properties |
| `0x1008` | `GetObjectInfo` | Get file metadata from camera |
| `0x1009` | `GetObject` | Download file from camera |

### Key Device Property Codes (from `PTPDef.h`)
| Code | Name | Usage |
|------|------|-------|
| `0xD2C1` | `DPC_S1_BUTTON` | Half-press shutter (autofocus) |
| `0xD2C2` | `DPC_S2_BUTTON` | Full-press shutter (capture) |
| `0xD2C8` | `DPC_MOVIE_REC` | Start/stop movie recording |
| `0xD20D` | `DPC_SHUTTER_SPEED` | Shutter speed control |
| `0x5007` | `DPC_FNUMBER` | Aperture (F-number) control |
| `0xD21E` | `DPC_ISO` | ISO sensitivity |
| `0x5013` | `DPC_DRIVE_MODE` | Drive mode (Normal, Continuous, Timer, etc.) |
| `0x500E` | `DPC_EXPOSURE_MODE` | Exposure mode (M, P, A, S) |
| `0x5005` | `DPC_WHITE_BALANCE` | White balance |
| `0xD222` | `DPC_SAVE_MEDIA` | Save target (Host/Memory Card/Both) |
| `0xD221` | `DPC_LIVEVIEW_STATUS` | Live view on/off status |
| `0xD2D4` | `DPC_NORMAL` | Normal capture button |

### Special Object Handles
| Handle | Purpose |
|--------|---------|
| `0xFFFFC001` | Last captured photo (JPEG/RAW) |
| `0xFFFFC002` | Current live view frame (JPEG) |

### Control Device Values
- `DOWN = 0x0002` — Press/activate a button
- `UP = 0x0001` — Release a button

## Drive Mode Values (from `DevicePropItemList.h`)
| Value | Mode |
|-------|------|
| `0x0001` | Normal (Single Shot) |
| `0x0002` | Continuous Shooting Hi |
| `0x8010` | Continuous Shooting Hi+ |
| `0x8012` | Continuous Shooting Lo |
| `0x8003` | Self Timer 5 sec |
| `0x8004` | Self Timer 10 sec |
| `0x8005` | Self Timer 2 sec |

## Architecture Diagram

```
┌──────────────────────┐
│  SONY A7III (ILCE-7M3)│
│  (USB / PC Remote)    │
└──────────┬───────────┘
           │ PTP/WIA Protocol
           │
┌──────────▼───────────────────────────────────────┐
│  CAMERA SERVICE (C++ / CameraService.exe)         │
│                                                   │
│  sony_ptp/                                        │
│  ├── PTPControl   → Send PTP commands to camera   │
│  ├── DataManager  → Parse camera state/properties │
│  └── PTPDef       → Protocol constants            │
│                                                   │
│  src/main.cpp                                     │
│  └── httplib Server (port 8080)                   │
│      ├── GET  /              Health check         │
│      ├── GET  /camera-info   List cameras         │
│      ├── POST /connect       SDIO handshake       │
│      ├── POST /capture       Take photo           │
│      ├── POST /set-save-path Set output dir       │
│      ├── GET  /images/*      Serve photos         │
│      └── GET  /liveview      MJPEG stream (TBD)   │
└──────────┬───────────────────────────────────────┘
           │ HTTP / REST API (localhost:8080)
           │
┌──────────▼───────────┐
│  UI / FRONTEND        │
│  (React, Vue, HTML)   │
│  + Payment Gateway    │
│  (Midtrans, Xendit)   │
└──────────────────────┘
```

## TODO / Roadmap
- [ ] **Live View (MJPEG Stream):** Implement `GET /liveview` using `GetObject(0xFFFFC002)` in a loop, streaming JPEG frames as `multipart/x-mixed-replace`
- [ ] **Camera Settings API:** Endpoints to get/set ISO, Aperture, Shutter Speed, White Balance via `SDIOSetExtDevicePropValue`
- [ ] **Camera Status Polling:** Background thread calling `SDIOGetAllExtDevicePropInfo` to keep camera state updated
- [ ] **Disconnect Endpoint:** `POST /disconnect` to cleanly release camera connection
- [ ] **Error Recovery:** Auto-reconnect on camera disconnect
- [ ] **JSON Library:** Replace manual JSON string building with a proper library (e.g., nlohmann/json)
- [ ] **Payment Gateway Integration:** Separate service or integrated endpoint for Midtrans/Xendit
- [ ] **Photo Overlay/Editing:** Post-processing pipeline for photobox templates

## Reference Documents
- `Camera Control PTP 2 Reference.pdf` — Sony PTP protocol specification (command codes, data formats, sequences)
- `Camera Control PTP Example Instruction Manual.pdf` — How to use the example project
- `README.pdf` — Sony's original readme

## Important Notes
1. Camera must be in **PC Remote** mode, NOT Mass Storage mode
2. Only **one application** can connect to the camera at a time via WIA
3. The service uses **COM (CoInitializeEx)** — thread safety matters
4. `PTPControl` is a **singleton** — access via `PTPControl::getInstance()`
5. All PTP commands use a **mutex** (`PTPMUTEX`) for thread safety
6. The original Sony example was an MFC (Microsoft Foundation Classes) desktop app — we stripped all UI dependencies and kept only the PTP communication core
