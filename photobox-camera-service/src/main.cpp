#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <cstdint>
#include <set>

// Windows definitions
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "httplib.h"
#include "../include/json.hpp"
#include "../sony_ptp/PTPControl.h"

using json = nlohmann::json;

// ========================================================
// Global state
// ========================================================
static std::atomic<bool> g_cameraConnected{false};
static std::mutex g_cameraMutex;
static std::string g_savePath = "C:\\photobox\\captures";

const DWORD SDIO_CONNECT_ID = 0x00000000;
const DWORD SDI_EXT_VERSION = 0xC8;
const DWORD LIVEVIEW_OBJECT_HANDLE = 0xFFFFC002;

// ========================================================
// Background polling state
// ========================================================
static CameraStatus g_cameraStatus;
static std::mutex g_statusMutex;
static std::atomic<bool> g_pollingActive{false};
static std::thread g_pollingThread;
const int POLL_INTERVAL_MS = 1000;

// ========================================================
// Auto-reconnect state
// ========================================================
const int RECONNECT_INTERVAL_MS = 30000;
const int RECONNECT_MAX_ATTEMPTS = 3;
static std::atomic<bool> g_manualDisconnect{false};
static std::atomic<bool> g_reconnectWorkerActive{false};
static std::thread g_reconnectThread;
static std::mutex g_reconnectStatusMutex;
static std::string g_reconnectState = "idle";
static int g_reconnectAttempt = 0;
static std::string g_reconnectMessage;
static std::string g_reconnectLastUpdated;

// Helper: Create directories recursively (like mkdir -p)
void createDirectoriesRecursive(const std::string& path) {
    size_t pos = 0;
    std::string current;
    while ((pos = path.find_first_of("\\/", pos + 1)) != std::string::npos) {
        current = path.substr(0, pos);
        CreateDirectoryA(current.c_str(), NULL);
    }
    CreateDirectoryA(path.c_str(), NULL);
}

// ========================================================
// Helper: Generate timestamped filename
// ========================================================
std::string generateFilename() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t_now);
    
    std::ostringstream oss;
    oss << "capture_"
        << std::put_time(&timeinfo, "%Y%m%d_%H%M%S")
        << "_" << std::setfill('0') << std::setw(3) << ms.count()
        << ".jpg";
    return oss.str();
}

std::string makeIsoTimestampNow() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t_now);

    std::ostringstream oss;
    oss << std::put_time(&timeinfo, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

void updateReconnectStatus(const std::string& state, int attempt, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_reconnectStatusMutex);
    g_reconnectState = state;
    g_reconnectAttempt = attempt;
    g_reconnectMessage = message;
    g_reconnectLastUpdated = makeIsoTimestampNow();
}

std::string escapeJsonString(const std::string& input) {
    std::string out;
    out.reserve(input.size() * 2);
    for (char c : input) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    return out;
}

bool isSafeImageFilename(const std::string& filename) {
    if (filename.empty() || filename.size() > 255) {
        return false;
    }

    if (filename.find("..") != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find('/') != std::string::npos ||
        filename.find(':') != std::string::npos) {
        return false;
    }

    for (char c : filename) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }

    const size_t dotPos = filename.find_last_of('.');
    if (dotPos == std::string::npos || dotPos == filename.size() - 1) {
        return false;
    }

    std::string ext = filename.substr(dotPos + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == "jpg" || ext == "jpeg";
}

std::string normalizeEscapedJsonBody(const std::string& body) {
    // Accept payloads like {\"path\":\"X:\\\\foo\"} from shell escaping.
    std::string normalized;
    normalized.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size() && body[i + 1] == '"') {
            continue;
        }
        normalized += body[i];
    }
    return normalized;
}

bool extractPathFromBody(const std::string& body, std::string& outPath) {
    const std::string normalized = normalizeEscapedJsonBody(body);
    const size_t keyPos = normalized.find("\"path\"");
    if (keyPos == std::string::npos) {
        return false;
    }

    const size_t colonPos = normalized.find(':', keyPos + 6);
    if (colonPos == std::string::npos) {
        return false;
    }

    const size_t start = normalized.find('"', colonPos + 1);
    if (start == std::string::npos) {
        return false;
    }

    std::string parsed;
    for (size_t i = start + 1; i < normalized.size(); ++i) {
        const char c = normalized[i];
        if (c == '"') {
            outPath = parsed;
            return true;
        }

        if (c == '\\' && i + 1 < normalized.size()) {
            const char next = normalized[i + 1];
            if (next == '\\' || next == '"' || next == '/') {
                parsed += next;
            } else {
                // Keep unknown escapes as-is so Windows paths like X:\test remain intact.
                parsed += '\\';
                parsed += next;
            }
            ++i;
            continue;
        }

        parsed += c;
    }

    return false;
}

bool tryExtractSettingValueU16(const json& payload, const char* key, uint16_t& outValue) {
    if (!payload.contains(key)) {
        return false;
    }

    const json& node = payload.at(key);
    const json* valueNode = &node;

    if (node.is_object() && node.contains("value")) {
        valueNode = &node.at("value");
    }

    if (!valueNode->is_number_integer() && !valueNode->is_number_unsigned()) {
        return false;
    }

    long long raw = valueNode->get<long long>();
    if (raw < 0 || raw > 0xFFFF) {
        return false;
    }

    outValue = static_cast<uint16_t>(raw);
    return true;
}

// ========================================================
// Connect to camera (full SDIO handshake)
// ========================================================
bool connectCamera() {
    std::lock_guard<std::mutex> lock(g_cameraMutex);

    if (g_cameraConnected.load()) {
        return true;
    }
    
    PTPControl& ptp = PTPControl::getInstance();

    // Step 1: Prepare WIA connection
    if (!ptp.prepareConnection()) {
        std::cerr << "[Camera] prepareConnection failed." << std::endl;
        return false;
    }
    std::cout << "[Camera] WIA connection established." << std::endl;

    // Step 2: SDIO Connect phase 1
    // Retry up to 3 times with a short delay in case the WIA session needs to settle
    // (can happen if service was restarted while camera still held a previous session).
    HRESULT hr = E_FAIL;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        hr = ptp.SDIOConnect(1, SDIO_CONNECT_ID, SDIO_CONNECT_ID);
        if (hr == S_OK) break;
        std::cerr << "[Camera] SDIOConnect(1) attempt " << attempt
                  << " failed hr=0x" << std::hex << hr << std::dec << std::endl;
        if (attempt < 3) {
            // Release and re-acquire WIA device before retrying
            if (!ptp.prepareConnection()) {
                std::cerr << "[Camera] prepareConnection retry failed." << std::endl;
                return false;
            }
            Sleep(500);
        }
    }
    if (hr != S_OK) {
        std::cerr << "[Camera] SDIOConnect(1) failed after retries hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Step 3: SDIO Connect phase 2
    hr = ptp.SDIOConnect(2, SDIO_CONNECT_ID, SDIO_CONNECT_ID);
    if (hr != S_OK) {
        std::cerr << "[Camera] SDIOConnect(2) failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Step 4: Get Extension Version
    hr = ptp.SDIOGetExtDeviceInfo(SDI_EXT_VERSION);
    if (hr != S_OK) {
        std::cerr << "[Camera] SDIOGetExtDeviceInfo failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Step 5: SDIO Connect phase 3
    hr = ptp.SDIOConnect(3, SDIO_CONNECT_ID, SDIO_CONNECT_ID);
    if (hr != S_OK) {
        std::cerr << "[Camera] SDIOConnect(3) failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    Sleep(200);
    g_cameraConnected = true;
    g_manualDisconnect = false;
    std::cout << "[Camera] Fully connected and ready!" << std::endl;

    if (g_pollingThread.joinable()) {
        g_pollingThread.join();
    }

    // Start background polling thread
    g_pollingActive = true;
    g_pollingThread = std::thread([]() {
        PTPControl& ptp = PTPControl::getInstance();
        DataManager& dmgr = DataManager::getInstance();

        while (g_pollingActive.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(POLL_INTERVAL_MS));

            if (!g_pollingActive.load() || !g_cameraConnected) continue;

            CameraStatus tempStatus;
            {
                std::lock_guard<std::mutex> cameraLock(g_cameraMutex);
                HRESULT hr = ptp.SDIOGetAllExtDevicePropInfo(NULL);
                if (hr == S_OK) {
                    dmgr.parseCameraDataHeadless(tempStatus);
                } else if (g_cameraConnected.load()) {
                    g_cameraConnected = false;
                    g_pollingActive = false;
                    g_manualDisconnect = false;
                    ptp.closeMutex();
                    updateReconnectStatus("attempting", 0, "Camera connection lost. Preparing reconnect attempts.");
                    std::cerr << "[Poll] Camera disconnected unexpectedly. hr=0x"
                              << std::hex << hr << std::dec << std::endl;
                    break;
                }
            }

            if (tempStatus.is_valid) {
                tempStatus.last_updated = makeIsoTimestampNow();

                std::lock_guard<std::mutex> statusLock(g_statusMutex);
                g_cameraStatus = tempStatus;
            }
        }
        std::cout << "[Poll] Polling thread stopped." << std::endl;
    });
    std::cout << "[Camera] Background polling started (interval: "
              << POLL_INTERVAL_MS << "ms)." << std::endl;
    updateReconnectStatus("success", 0, "Camera connected.");
    return true;
}

// ========================================================
// Disconnect from camera
// ========================================================
void disconnectCamera(bool manualDisconnect = true) {
    // Stop polling first to avoid holding g_cameraMutex during join
    g_pollingActive = false;
    if (g_pollingThread.joinable()) {
        g_pollingThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_cameraMutex);
        if (!g_cameraConnected) return;
        PTPControl::getInstance().closeMutex();
        g_cameraConnected = false;
        g_manualDisconnect = manualDisconnect;
    }
    std::cout << "[Camera] Disconnected." << std::endl;
    if (manualDisconnect) {
        updateReconnectStatus("idle", 0, "Disconnected by user.");
    }
}

void startReconnectWorker() {
    if (g_reconnectThread.joinable()) {
        return;
    }

    g_reconnectWorkerActive = true;
    g_reconnectThread = std::thread([]() {
        bool recovering = false;
        int attempt = 0;
        auto nextAttemptAt = std::chrono::steady_clock::now();

        while (g_reconnectWorkerActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            if (g_manualDisconnect.load()) {
                recovering = false;
                attempt = 0;
                continue;
            }

            if (g_cameraConnected.load()) {
                recovering = false;
                attempt = 0;
                continue;
            }

            if (!recovering) {
                recovering = true;
                attempt = 0;
                nextAttemptAt = std::chrono::steady_clock::now();
                updateReconnectStatus("attempting", attempt, "Auto-reconnect started.");
            }

            if (attempt >= RECONNECT_MAX_ATTEMPTS) {
                updateReconnectStatus("failed", attempt, "Auto-reconnect failed after max attempts.");
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }

            if (std::chrono::steady_clock::now() < nextAttemptAt) {
                continue;
            }

            ++attempt;
            updateReconnectStatus("attempting", attempt, "Reconnecting camera...");
            std::cout << "[Reconnect] Attempt " << attempt << "/" << RECONNECT_MAX_ATTEMPTS << std::endl;

            if (connectCamera()) {
                recovering = false;
                attempt = 0;
                updateReconnectStatus("success", 0, "Camera reconnected successfully.");
                std::cout << "[Reconnect] Success." << std::endl;
                continue;
            }

            std::ostringstream msg;
            msg << "Reconnect attempt " << attempt << " failed.";
            updateReconnectStatus("attempting", attempt, msg.str());
            nextAttemptAt = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(RECONNECT_INTERVAL_MS);
        }
    });
}

// ========================================================
// Capture photo (Normal drive mode - single shot)
// ========================================================
struct CaptureResult {
    bool success;
    std::string message;
    std::string filePath;
    std::string fileName;
};

CaptureResult capturePhoto() {
    CaptureResult result = {false, "", "", ""};
    std::lock_guard<std::mutex> lock(g_cameraMutex);

    if (!g_cameraConnected) {
        result.message = "Camera not connected";
        return result;
    }

    PTPControl& ptp = PTPControl::getInstance();
    HRESULT hr;

    // Step 1: Half-press shutter (S1) for autofocus
    hr = ptp.SDIOControlDevice(DPC_S1_BUTTON, 0x0002); // DOWN
    if (hr != S_OK) {
        result.message = "S1 (half-press) DOWN failed";
        return result;
    }
    std::cout << "[Capture] S1 DOWN (autofocus)..." << std::endl;
    Sleep(500); // Give time for autofocus to lock

    // Step 2: Full-press shutter (S2) to take the photo
    hr = ptp.SDIOControlDevice(DPC_S2_BUTTON, 0x0002); // DOWN
    if (hr != S_OK) {
        // Release S1 before returning
        ptp.SDIOControlDevice(DPC_S1_BUTTON, 0x0001); // UP
        result.message = "S2 (full-press) DOWN failed";
        return result;
    }
    std::cout << "[Capture] S2 DOWN (shutter fired)..." << std::endl;
    Sleep(200);

    // Step 3: Release shutter buttons
    hr = ptp.SDIOControlDevice(DPC_S2_BUTTON, 0x0001); // UP
    if (hr != S_OK) {
        std::cerr << "[Capture] Warning: S2 UP failed" << std::endl;
    }
    hr = ptp.SDIOControlDevice(DPC_S1_BUTTON, 0x0001); // UP
    if (hr != S_OK) {
        std::cerr << "[Capture] Warning: S1 UP failed" << std::endl;
    }
    std::cout << "[Capture] Shutter released." << std::endl;

    // Step 4: Wait for camera to process the image
    Sleep(1000);

    // Step 5: Get object info for the shot image
    PTP_GetObjectInfo info = {0};
    hr = ptp.GetObjectInfo(SHOT_OBJECT_HANDLE, info);
    if (hr != S_OK) {
        result.message = "GetObjectInfo failed - image may not be ready yet";
        return result;
    }
    std::cout << "[Capture] Image size: " << info.ObjCompSz << " bytes" << std::endl;

    // Step 6: Download the image data
    std::vector<BYTE> buffer(info.ObjCompSz);
    hr = ptp.ExecuteGetObject(SHOT_OBJECT_HANDLE, buffer.data(), info.ObjCompSz);
    if (hr != S_OK) {
        result.message = "ExecuteGetObject failed - could not download image";
        return result;
    }

    // Step 7: Save image to disk
    // Create save directory if it doesn't exist
    createDirectoriesRecursive(g_savePath);

    std::string fileName = generateFilename();
    std::string fullPath = g_savePath + "\\" + fileName;

    HANDLE hFile = CreateFileA(fullPath.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        result.message = "Failed to create output file: " + fullPath;
        return result;
    }

    DWORD dwBytesWritten = 0;
    BOOL writeOk = WriteFile(hFile, buffer.data(), info.ObjCompSz, &dwBytesWritten, NULL);
    CloseHandle(hFile);

    if (!writeOk) {
        result.message = "Failed to write image data to file";
        return result;
    }

    std::cout << "[Capture] Image saved: " << fullPath << std::endl;

    result.success = true;
    result.message = "Photo captured successfully";
    result.filePath = fullPath;
    result.fileName = fileName;
    return result;
}

// ========================================================
// Main - HTTP Server
// ========================================================
int main() {
    std::cout << "=== Photobox Camera Service ===" << std::endl;
    std::cout << "Save path: " << g_savePath << std::endl;
    updateReconnectStatus("idle", 0, "Waiting for camera connection.");

    // Initialize COM for WIA
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    httplib::Server svr;
    startReconnectWorker();

    // Global CORS headers for all responses (allows file:// and any origin)
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });

    // CORS preflight handler
    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // Health check
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::string json = "{\"status\":\"ok\","
            "\"message\":\"Camera Service Running\","
            "\"camera_connected\":" + std::string(g_cameraConnected ? "true" : "false") + "}";
        res.set_content(json, "application/json");
    });

    // List connected cameras
    svr.Get("/camera-info", [](const httplib::Request&, httplib::Response& res) {
        std::vector<std::string> cameras = PTPControl::getInstance().getConnectedCameras();

        std::string json = "{\"cameras\": [";
        for (size_t i = 0; i < cameras.size(); ++i) {
            json += "\"" + cameras[i] + "\"";
            if (i < cameras.size() - 1) json += ", ";
        }
        json += "], \"connected\": " + std::string(g_cameraConnected ? "true" : "false") + "}";
        res.set_content(json, "application/json");
    });

    // Connect to camera
    svr.Post("/connect", [](const httplib::Request&, httplib::Response& res) {
        if (g_cameraConnected) {
            res.set_content("{\"status\":\"ok\", \"message\":\"Already connected\"}", "application/json");
            return;
        }
        g_manualDisconnect = false;
        bool ok = connectCamera();
        if (ok) {
            res.set_content("{\"status\":\"ok\", \"message\":\"Camera connected successfully\"}", "application/json");
        } else {
            res.status = 500;
            res.set_content("{\"status\":\"error\", \"message\":\"Failed to connect to camera\"}", "application/json");
        }
    });

    // Disconnect from camera
    svr.Post("/disconnect", [](const httplib::Request&, httplib::Response& res) {
        if (!g_cameraConnected) {
            res.set_content("{\"status\":\"ok\",\"message\":\"Camera already disconnected\"}", "application/json");
            return;
        }
        disconnectCamera(true);
        res.set_content("{\"status\":\"ok\",\"message\":\"Camera disconnected\"}", "application/json");
    });

    // Capture photo (Normal drive mode - single shot)
    svr.Post("/capture", [](const httplib::Request&, httplib::Response& res) {
        if (!g_cameraConnected) {
            res.status = 400;
            res.set_content("{\"status\":\"error\", \"message\":\"Camera not connected. Call POST /connect first.\"}", "application/json");
            return;
        }

        CaptureResult result = capturePhoto();

        if (result.success) {
            std::string json = "{\"status\":\"ok\","
                "\"message\":\"" + result.message + "\","
                "\"file_name\":\"" + result.fileName + "\","
                "\"file_path\":\"" + result.filePath + "\"}";
            // Escape backslashes in file_path for JSON
            std::string jsonFixed;
            for (char c : json) {
                if (c == '\\') jsonFixed += "\\\\";
                else jsonFixed += c;
            }
            res.set_content(jsonFixed, "application/json");
        } else {
            res.status = 500;
            res.set_content("{\"status\":\"error\", \"message\":\"" + result.message + "\"}", "application/json");
        }
    });

    // Serve captured images with no-cache headers.
    // (set_mount_point adds Cache-Control: max-age which causes browser to show stale thumbnails)
    svr.Get(R"(/images/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        if (req.matches.size() < 2) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Missing filename\"}", "application/json");
            return;
        }
        std::string filename = req.matches[1].str();
        if (!isSafeImageFilename(filename)) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Invalid filename\"}", "application/json");
            return;
        }
        std::string fullPath = g_savePath + "\\" + filename;
        std::ifstream file(fullPath, std::ios::binary);
        if (!file) {
            res.status = 404;
            res.set_content("{\"status\":\"error\",\"message\":\"File not found\"}", "application/json");
            return;
        }
        std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        res.set_header("Cache-Control", "no-store");
        res.set_content(data, "image/jpeg");
    });

    // Set save path
    svr.Post("/set-save-path", [](const httplib::Request& req, httplib::Response& res) {
        if (req.body.empty()) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Body required: {\\\"path\\\":\\\"X:\\\\\\\\your-path\\\"}\"}", "application/json");
            return;
        }

        std::string parsedPath;
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            body = json::parse(normalizeEscapedJsonBody(req.body), nullptr, false);
        }

        if (!body.is_discarded() && body.contains("path") && body["path"].is_string()) {
            parsedPath = body["path"].get<std::string>();
        } else if (!extractPathFromBody(req.body, parsedPath)) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Invalid JSON body. Expected field: path\"}", "application/json");
            return;
        }

        if (parsedPath.empty()) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Path cannot be empty\"}", "application/json");
            return;
        }

        g_savePath = parsedPath;
        createDirectoriesRecursive(g_savePath);

        json out = {
            {"status", "ok"},
            {"save_path", g_savePath}
        };
        res.set_content(out.dump(), "application/json");
    });

    // Get current save path
    svr.Get("/save-path", [](const httplib::Request&, httplib::Response& res) {
        json out = {
            {"status", "ok"},
            {"save_path", g_savePath}
        };
        res.set_content(out.dump(), "application/json");
    });

    // Camera status (background polling cache)
    svr.Get("/camera-status", [](const httplib::Request&, httplib::Response& res) {
        json reconnect;
        {
            std::lock_guard<std::mutex> reconnectLock(g_reconnectStatusMutex);
            reconnect = {
                {"state", g_reconnectState},
                {"attempt", g_reconnectAttempt},
                {"max_attempts", RECONNECT_MAX_ATTEMPTS},
                {"interval_ms", RECONNECT_INTERVAL_MS},
                {"message", g_reconnectMessage},
                {"last_updated", g_reconnectLastUpdated}
            };
        }

        std::lock_guard<std::mutex> lock(g_statusMutex);
        if (!g_cameraStatus.is_valid) {
            json out = {
                {"status", "ok"},
                {"is_valid", false},
                {"message", "No data yet. Connect camera and wait for first poll."},
                {"reconnect", reconnect}
            };
            res.set_content(out.dump(), "application/json");
            return;
        }

        const CameraStatus& s = g_cameraStatus;
        json out = {
            {"status", "ok"},
            {"is_valid", true},
            {"last_updated", s.last_updated},
            {"iso", {{"value", s.iso_value}, {"label", s.iso_label}}},
            {"fnumber", {{"value", s.fnumber_value}, {"label", s.fnumber_label}}},
            {"shutter_speed", {{"value", s.shutter_value}, {"label", s.shutter_label}}},
            {"exposure_mode", {{"value", s.exposure_mode_value}, {"label", s.exposure_mode_label}}},
            {"white_balance", {{"value", s.white_balance_value}, {"label", s.white_balance_label}}},
            {"drive_mode", {{"value", s.drive_mode_value}, {"label", s.drive_mode_label}}},
            {"battery_level", {{"value", s.battery_level_value}, {"label", s.battery_level_label}}},
            {"battery_charge", s.battery_charge_value},
            {"af_status", {{"value", s.af_status_value}, {"label", s.af_status_label}}},
            {"liveview_active", s.liveview_status_value == 1},
            {"focus_mode", {{"value", s.focus_mode_value}, {"label", s.focus_mode_label}}},
            {"reconnect", reconnect}
        };
        res.set_content(out.dump(), "application/json");
    });

    // Read camera settings snapshot (from background polling cache)
    svr.Get("/camera/settings", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        if (!g_cameraStatus.is_valid) {
            json out = {
                {"status", "error"},
                {"message", "Camera settings are not available yet. Connect camera and wait for first poll."}
            };
            res.status = 503;
            res.set_content(out.dump(), "application/json");
            return;
        }

        const CameraStatus& s = g_cameraStatus;
        json out = {
            {"status", "ok"},
            {"last_updated", s.last_updated},
            {"iso", {{"value", s.iso_value}, {"label", s.iso_label}}},
            {"fnumber", {{"value", s.fnumber_value}, {"label", s.fnumber_label}}},
            {"shutter_speed", {{"value", s.shutter_value}, {"label", s.shutter_label}}},
            {"white_balance", {{"value", s.white_balance_value}, {"label", s.white_balance_label}}},
            {"drive_mode", {{"value", s.drive_mode_value}, {"label", s.drive_mode_label}}}
        };
        res.set_content(out.dump(), "application/json");
    });

    // Apply camera settings (supports raw numeric value or object {"value": <number>})
    svr.Post("/camera/settings", [](const httplib::Request& req, httplib::Response& res) {
        if (!g_cameraConnected) {
            json out = {
                {"status", "error"},
                {"message", "Camera not connected. Call POST /connect first."}
            };
            res.status = 400;
            res.set_content(out.dump(), "application/json");
            return;
        }

        json payload = json::parse(req.body, nullptr, false);
        if (payload.is_discarded()) {
            payload = json::parse(normalizeEscapedJsonBody(req.body), nullptr, false);
        }
        if (payload.is_discarded() || !payload.is_object()) {
            json out = {
                {"status", "error"},
                {"message", "Invalid JSON body."}
            };
            res.status = 400;
            res.set_content(out.dump(), "application/json");
            return;
        }

        struct SettingSpec {
            const char* key;
            DWORD propertyCode;
        };

        const SettingSpec specs[] = {
            {"iso", DPC_ISO},
            {"fnumber", DPC_FNUMBER},
            {"aperture", DPC_FNUMBER},
            {"shutter_speed", DPC_SHUTTER_SPEED},
            {"shutter", DPC_SHUTTER_SPEED},
            {"white_balance", DPC_WHITE_BALANCE},
            {"wb", DPC_WHITE_BALANCE},
            {"drive_mode", DPC_DRIVE_MODE}
        };

        json applied = json::array();
        std::set<DWORD> updatedProps;
        std::lock_guard<std::mutex> cameraLock(g_cameraMutex);
        PTPControl& ptp = PTPControl::getInstance();

        for (const auto& spec : specs) {
            uint16_t value = 0;
            if (!tryExtractSettingValueU16(payload, spec.key, value)) {
                continue;
            }
            if (updatedProps.find(spec.propertyCode) != updatedProps.end()) {
                continue;
            }

            HRESULT hr = ptp.SDIOSetExtDevicePropValue(spec.propertyCode, value, sizeof(value));
            if (hr != S_OK) {
                json out = {
                    {"status", "error"},
                    {"message", std::string("Failed to set ") + spec.key},
                    {"property", spec.key},
                    {"hresult", hr}
                };
                res.status = 500;
                res.set_content(out.dump(), "application/json");
                return;
            }

            applied.push_back({
                {"property", spec.key},
                {"value", value}
            });
            updatedProps.insert(spec.propertyCode);
        }

        if (applied.empty()) {
            json out = {
                {"status", "error"},
                {"message", "No valid settings found. Use: iso, fnumber/aperture, shutter_speed/shutter, white_balance/wb, drive_mode."}
            };
            res.status = 400;
            res.set_content(out.dump(), "application/json");
            return;
        }

        json out = {
            {"status", "ok"},
            {"message", "Settings applied"},
            {"applied", applied}
        };
        res.set_content(out.dump(), "application/json");
    });

    // Wait for camera to report liveview is active (it auto-enables in PC Remote mode).
    // DPC_LIVEVIEW_STATUS is read-only; we just poll until the background thread sees value==1.
    svr.Post("/liveview/start", [](const httplib::Request&, httplib::Response& res) {
        if (!g_cameraConnected) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Camera not connected. Call POST /connect first.\"}", "application/json");
            return;
        }

        PTPControl& ptp = PTPControl::getInstance();
        const int maxWaitMs = 3000;
        const int stepMs = 100;
        int waited = 0;
        BOOL flag = ptp.getIsLiveviewValidFlag();
        while (!flag && waited < maxWaitMs) {
            Sleep(stepMs);
            waited += stepMs;
            flag = ptp.getIsLiveviewValidFlag();
        }
        std::cout << "[Liveview/start] waited=" << waited << "ms isLiveviewValidFlag="
                  << (flag ? "TRUE" : "FALSE") << std::endl;

        if (!flag) {
            res.status = 503;
            res.set_content("{\"status\":\"error\",\"message\":\"Camera did not report liveview active within timeout. Ensure camera is in PC Remote / Shoot mode.\",\"liveview_valid_flag\":false}", "application/json");
            return;
        }
        res.set_content("{\"status\":\"ok\",\"message\":\"Liveview active\",\"liveview_valid_flag\":true}", "application/json");
    });

    // Liveview stop: no-op on the camera side (DPC_LIVEVIEW_STATUS is read-only).
    // The camera deactivates liveview when it leaves PC Remote / Shoot mode.
    svr.Post("/liveview/stop", [](const httplib::Request&, httplib::Response& res) {
        if (!g_cameraConnected) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Camera not connected.\"}", "application/json");
            return;
        }
        std::cout << "[Liveview/stop] no-op (DPC_LIVEVIEW_STATUS is read-only)" << std::endl;
        res.set_content("{\"status\":\"ok\",\"message\":\"Liveview stop is a client-side no-op; stop polling /liveview/frame to free bandwidth.\"}", "application/json");
    });

    // Fetch one liveview frame as JPEG
    svr.Get("/liveview/frame", [](const httplib::Request&, httplib::Response& res) {
        if (!g_cameraConnected) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Camera not connected. Call POST /connect first.\"}", "application/json");
            return;
        }

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");

        std::lock_guard<std::mutex> lock(g_cameraMutex);
        PTPControl& ptp = PTPControl::getInstance();

        BOOL lvValid = ptp.getIsLiveviewValidFlag();
        PTP_GetObjectInfo info = {0};
        HRESULT hr = ptp.GetObjectInfo(LIVEVIEW_OBJECT_HANDLE, info);
        std::cout << "[Liveview/frame] lvValid=" << (lvValid ? "T" : "F")
                  << " GetObjectInfo hr=0x" << std::hex << hr << std::dec
                  << " ObjCompSz=" << info.ObjCompSz << std::endl;
        if (hr != S_OK || info.ObjCompSz == 0) {
            res.status = 503;
            std::ostringstream msg;
            msg << "{\"status\":\"error\",\"message\":\"Liveview frame is not available\","
                << "\"liveview_valid_flag\":" << (lvValid ? "true" : "false") << ","
                << "\"hresult\":\"0x" << std::hex << hr << "\","
                << "\"obj_comp_sz\":" << std::dec << info.ObjCompSz << "}";
            res.set_content(msg.str(), "application/json");
            return;
        }

        std::vector<BYTE> buffer(info.ObjCompSz);
        hr = ptp.ExecuteGetObject(LIVEVIEW_OBJECT_HANDLE, buffer.data(), info.ObjCompSz);
        if (hr != S_OK) {
            std::cout << "[Liveview/frame] ExecuteGetObject FAILED hr=0x" << std::hex << hr << std::dec << std::endl;
            res.status = 503;
            res.set_content("{\"status\":\"error\",\"message\":\"Failed to get liveview frame\"}", "application/json");
            return;
        }

        // Sony liveview buffer layout: [DWORD Offset][DWORD Size][...padding...][JPEG payload]
        // Actual JPEG starts at buffer[Offset] with length Size.
        if (info.ObjCompSz < 8) {
            res.status = 503;
            res.set_content("{\"status\":\"error\",\"message\":\"Liveview buffer too small\"}", "application/json");
            return;
        }
        DWORD jpegOffset = *reinterpret_cast<DWORD*>(buffer.data());
        DWORD jpegSize   = *reinterpret_cast<DWORD*>(buffer.data() + 4);
        if (jpegOffset + jpegSize > info.ObjCompSz || jpegSize == 0) {
            std::cout << "[Liveview/frame] invalid header offset=" << jpegOffset
                      << " size=" << jpegSize << " total=" << info.ObjCompSz << std::endl;
            res.status = 503;
            res.set_content("{\"status\":\"error\",\"message\":\"Invalid liveview header\"}", "application/json");
            return;
        }

        res.set_content(reinterpret_cast<const char*>(buffer.data() + jpegOffset),
                        static_cast<size_t>(jpegSize), "image/jpeg");
    });

    // Full MJPEG stream: multipart/x-mixed-replace
    svr.Get("/liveview", [](const httplib::Request&, httplib::Response& res) {
        if (!g_cameraConnected) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Camera not connected. Call POST /connect first.\"}", "application/json");
            return;
        }

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");

        const std::string boundary = "photoboxframe";
        const std::string contentType = "multipart/x-mixed-replace; boundary=" + boundary;

        res.set_chunked_content_provider(
            contentType,
            [boundary](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                PTPControl& ptp = PTPControl::getInstance();

                PTP_GetObjectInfo info = {0};
                std::vector<BYTE> buffer;
                DWORD size = 0;
                {
                    std::lock_guard<std::mutex> lock(g_cameraMutex);
                    HRESULT hr = ptp.GetObjectInfo(LIVEVIEW_OBJECT_HANDLE, info);
                    if (hr != S_OK || info.ObjCompSz == 0) {
                        Sleep(50);
                        return true; // keep stream open, try next frame
                    }
                    size = info.ObjCompSz;
                    buffer.resize(size);
                    hr = ptp.ExecuteGetObject(LIVEVIEW_OBJECT_HANDLE, buffer.data(), size);
                    if (hr != S_OK) {
                        Sleep(50);
                        return true;
                    }
                }

                // Strip Sony liveview header to get pure JPEG payload
                if (size < 8) { Sleep(50); return true; }
                DWORD jpegOffset = *reinterpret_cast<DWORD*>(buffer.data());
                DWORD jpegSize   = *reinterpret_cast<DWORD*>(buffer.data() + 4);
                if (jpegOffset + jpegSize > size || jpegSize == 0) {
                    Sleep(50); return true;
                }
                const BYTE* jpegData = buffer.data() + jpegOffset;

                std::ostringstream header;
                header << "--" << boundary << "\r\n"
                       << "Content-Type: image/jpeg\r\n"
                       << "Content-Length: " << jpegSize << "\r\n\r\n";
                const std::string headerStr = header.str();

                bool ok = sink.write(headerStr.data(), headerStr.size());
                if (ok) {
                    ok = sink.write(reinterpret_cast<const char*>(jpegData), jpegSize);
                }
                if (ok) {
                    ok = sink.write("\r\n", 2);
                }

                Sleep(33); // ~30fps cap
                return ok;
            });
    });

    // Delete a captured image file
    svr.Delete(R"(/images/(.+))", [](const httplib::Request& req, httplib::Response& res) {
        if (req.matches.size() < 2) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Missing filename\"}", "application/json");
            return;
        }

        std::string filename = req.matches[1].str();
        if (!isSafeImageFilename(filename)) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Invalid filename\"}", "application/json");
            return;
        }

        std::string fullPath = g_savePath + "\\" + filename;
        BOOL ok = DeleteFileA(fullPath.c_str());
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) {
                res.status = 404;
                res.set_content("{\"status\":\"error\",\"message\":\"File not found\"}", "application/json");
                return;
            }

            res.status = 500;
            res.set_content("{\"status\":\"error\",\"message\":\"Failed to delete file\"}", "application/json");
            return;
        }

        res.set_content(
            "{\"status\":\"ok\",\"message\":\"File deleted\",\"file_name\":\"" +
                escapeJsonString(filename) + "\"}",
            "application/json");
    });

    std::cout << "HTTP Server starting on http://localhost:8080" << std::endl;
    std::cout << "\nAvailable endpoints:" << std::endl;
    std::cout << "  GET  /              - Health check" << std::endl;
    std::cout << "  GET  /camera-info   - List connected cameras" << std::endl;
    std::cout << "  POST /connect       - Connect to camera (SDIO handshake)" << std::endl;
    std::cout << "  POST /disconnect    - Disconnect from camera" << std::endl;
    std::cout << "  POST /capture       - Take a photo (Normal drive mode)" << std::endl;
    std::cout << "  POST /set-save-path - Set save directory" << std::endl;
    std::cout << "  GET  /save-path     - Get current save directory" << std::endl;
    std::cout << "  GET  /images/<file> - Serve captured images" << std::endl;
    std::cout << "  DELETE /images/<file> - Delete captured image" << std::endl;
    std::cout << "  GET  /camera-status - Camera settings (background poll)" << std::endl;
    std::cout << "  GET  /camera/settings - Get current camera setting values" << std::endl;
    std::cout << "  POST /camera/settings - Apply camera settings" << std::endl;
    std::cout << "  POST /liveview/start - Enable liveview mode" << std::endl;
    std::cout << "  POST /liveview/stop - Disable liveview mode" << std::endl;
    std::cout << "  GET  /liveview/frame - Get single liveview JPEG frame" << std::endl;
    std::cout << "  GET  /liveview      - Live view stream (TBD)" << std::endl;
    
    svr.listen("0.0.0.0", 8080);

    // Shutdown polling thread cleanly
    g_pollingActive = false;
    if (g_pollingThread.joinable()) {
        g_pollingThread.join();
    }

    g_reconnectWorkerActive = false;
    if (g_reconnectThread.joinable()) {
        g_reconnectThread.join();
    }

    CoUninitialize();
    return 0;
}
