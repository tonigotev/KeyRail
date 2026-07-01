#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "browser_bridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include "synthetic_hotkey_suppression.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using nlohmann::json;

namespace {

constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct BrowserClient {
    SOCKET socket = INVALID_SOCKET;
    std::mutex sendMutex;
};

struct PendingResponse {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    bool ok = false;
    std::string message;
};

struct BridgeResult {
    bool sent = false;
    bool gotResponse = false;
    bool ok = false;
    std::string detail;
};

std::atomic<bool> g_running{false};
SOCKET g_listenSocket = INVALID_SOCKET;
std::thread g_acceptThread;
std::mutex g_clientsMutex;
std::vector<std::shared_ptr<BrowserClient>> g_clients;
unsigned short g_port = 8790;

std::mutex g_targetsMutex;
std::condition_variable g_targetsChanged;
std::vector<MediaTarget> g_targets;
unsigned long long g_targetsRevision = 0;
std::atomic<unsigned long long> g_nextMessageId{1};
std::mutex g_pendingMutex;
std::unordered_map<std::string, std::shared_ptr<PendingResponse>> g_pendingResponses;

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), size);
    out.pop_back();
    return out;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
        value.pop_back();
    }
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    return value;
}

std::string websocketKey(const std::string& request) {
    const std::string needle = "Sec-WebSocket-Key:";
    size_t start = request.find(needle);
    if (start == std::string::npos) return {};
    start += needle.size();
    size_t end = request.find("\r\n", start);
    if (end == std::string::npos) return {};
    return trim(request.substr(start, end - start));
}

bool sha1(const std::string& input, unsigned char out[20]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD written = 0;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &written, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    std::vector<unsigned char> object(objectLength);
    bool ok = BCryptCreateHash(alg, &hash, object.data(), objectLength, nullptr, 0, 0) == 0
        && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) == 0
        && BCryptFinishHash(hash, out, 20, 0) == 0;

    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string base64(const unsigned char* data, size_t size) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);

    for (size_t i = 0; i < size; i += 3) {
        unsigned int value = data[i] << 16;
        if (i + 1 < size) value |= data[i + 1] << 8;
        if (i + 2 < size) value |= data[i + 2];

        out.push_back(alphabet[(value >> 18) & 0x3f]);
        out.push_back(alphabet[(value >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? alphabet[(value >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < size ? alphabet[value & 0x3f] : '=');
    }
    return out;
}

std::string websocketAccept(const std::string& key) {
    unsigned char digest[20]{};
    if (!sha1(key + kWebSocketGuid, digest)) return {};
    return base64(digest, sizeof(digest));
}

bool sendAll(SOCKET socket, const char* data, int length) {
    int sent = 0;
    while (sent < length) {
        int result = send(socket, data + sent, length - sent, 0);
        if (result <= 0) return false;
        sent += result;
    }
    return true;
}

bool recvAll(SOCKET socket, unsigned char* data, size_t length) {
    size_t received = 0;
    while (received < length) {
        int result = recv(socket, reinterpret_cast<char*>(data + received), static_cast<int>(length - received), 0);
        if (result <= 0) return false;
        received += static_cast<size_t>(result);
    }
    return true;
}

bool sendTextFrame(const std::shared_ptr<BrowserClient>& client, const std::string& text) {
    std::lock_guard<std::mutex> lock(client->sendMutex);

    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    if (text.size() < 126) {
        frame.push_back(static_cast<unsigned char>(text.size()));
    } else if (text.size() <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>((text.size() >> 8) & 0xff));
        frame.push_back(static_cast<unsigned char>(text.size() & 0xff));
    } else {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<unsigned char>((text.size() >> shift) & 0xff));
        }
    }

    frame.insert(frame.end(), text.begin(), text.end());
    return sendAll(client->socket, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()));
}

bool readTextFrame(SOCKET socket, std::string* text) {
    unsigned char header[2]{};
    if (!recvAll(socket, header, sizeof(header))) return false;

    const unsigned char opcode = header[0] & 0x0f;
    const bool masked = (header[1] & 0x80) != 0;
    unsigned long long length = header[1] & 0x7f;

    if (opcode == 0x8) return false;
    if (opcode != 0x1) return true;

    if (length == 126) {
        unsigned char ext[2]{};
        if (!recvAll(socket, ext, sizeof(ext))) return false;
        length = (static_cast<unsigned long long>(ext[0]) << 8) | ext[1];
    } else if (length == 127) {
        unsigned char ext[8]{};
        if (!recvAll(socket, ext, sizeof(ext))) return false;
        length = 0;
        for (unsigned char byte : ext) length = (length << 8) | byte;
    }

    unsigned char mask[4]{};
    if (masked && !recvAll(socket, mask, sizeof(mask))) return false;
    if (length > 1024 * 1024) return false;

    std::string payload(static_cast<size_t>(length), '\0');
    if (length > 0 && !recvAll(socket, reinterpret_cast<unsigned char*>(payload.data()), static_cast<size_t>(length))) return false;

    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        }
    }

    *text = std::move(payload);
    return true;
}

std::string nextMessageId() {
    return "daemon-" + std::to_string(g_nextMessageId.fetch_add(1));
}

std::vector<std::shared_ptr<BrowserClient>> clientsSnapshot() {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    return g_clients;
}

void removeClient(const std::shared_ptr<BrowserClient>& client) {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), client), g_clients.end());
}

MediaTarget parseTarget(const json& value) {
    MediaTarget target;
    target.kind = L"browser";
    target.id = utf8ToWide(value.value("id", ""));
    target.appId = utf8ToWide(value.value("app", "Browser"));
    target.title = utf8ToWide(value.value("title", ""));
    target.artist = utf8ToWide(value.value("artist", ""));
    target.sourceHost = utf8ToWide(value.value("site", ""));
    target.favIconUrl = utf8ToWide(value.value("favIconUrl", ""));
    target.artworkUrl = utf8ToWide(value.value("artworkUrl", ""));
    target.url = utf8ToWide(value.value("url", ""));
    target.documentTitle = utf8ToWide(value.value("documentTitle", ""));
    target.tabTitle = utf8ToWide(value.value("tabTitle", ""));
    target.mediaSessionPlaybackState = utf8ToWide(value.value("mediaSessionPlaybackState", ""));
    target.playbackStatus = value.value("playing", false) ? L"playing" : L"paused";
    target.playing = value.value("playing", false);
    target.canTogglePlayPause = value.value("canPlayPause", true);
    target.canNext = value.value("canNext", false);
    target.canPrevious = value.value("canPrevious", false);
    return target;
}

void updateTargetsFromJson(const json& targetsJson) {
    if (!targetsJson.is_array()) return;

    std::vector<MediaTarget> targets;
    for (const auto& item : targetsJson) {
        if (!item.is_object()) continue;
        MediaTarget target = parseTarget(item);
        if (!target.id.empty()) targets.push_back(std::move(target));
    }

    {
        std::lock_guard<std::mutex> lock(g_targetsMutex);
        g_targets = std::move(targets);
        ++g_targetsRevision;
    }
    g_targetsChanged.notify_all();
}

void sendRequestTargets() {
    json request;
    request["id"] = nextMessageId();
    request["type"] = "request_targets";
    request["payload"] = json::object();
    std::string text = request.dump();

    for (const auto& client : clientsSnapshot()) {
        sendTextFrame(client, text);
    }
}

void completePendingResponse(const json& message) {
    const std::string id = message.value("id", "");
    if (id.empty()) return;

    std::shared_ptr<PendingResponse> pending;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        auto found = g_pendingResponses.find(id);
        if (found == g_pendingResponses.end()) return;
        pending = found->second;
    }

    std::string detail;
    auto error = message.find("error");
    if (error != message.end()) {
        if (error->is_object()) {
            detail = error->value("message", "");
        } else if (error->is_string()) {
            detail = error->get<std::string>();
        }
    }

    if (detail.empty()) {
        auto payload = message.find("payload");
        if (payload != message.end() && payload->is_object()) {
            detail = payload->value("message", "");
            const std::string reason = payload->value("reason", "");
            const std::string via = payload->value("via", "");
            if (!reason.empty()) {
                detail += detail.empty() ? "reason: " + reason : " (" + reason + ")";
            }
            if (!via.empty()) {
                detail += detail.empty() ? "via: " + via : " via " + via;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->ok = message.value("ok", false);
        pending->message = std::move(detail);
        pending->done = true;
    }
    pending->changed.notify_all();
}

void handleMessage(const std::shared_ptr<BrowserClient>& client, const std::string& text) {
    try {
        json message = json::parse(text);
        std::string type = message.value("type", "");

        if (type == "hello") {
            wprintf(L"browser bridge: extension says hello\n");
            sendRequestTargets();
            return;
        }

        if (type == "media_targets") {
            updateTargetsFromJson(message["payload"]["targets"]);
            return;
        }

        if (type == "targets_changed") {
            sendRequestTargets();
            return;
        }

        if (type == "response") {
            completePendingResponse(message);
            auto payload = message.find("payload");
            if (payload != message.end() && payload->is_object()) {
                auto targets = payload->find("targets");
                if (targets != payload->end()) updateTargetsFromJson(*targets);
            }
            return;
        }

        if (type == "ping") {
            json response;
            response["id"] = message.value("id", "");
            response["type"] = "pong";
            sendTextFrame(client, response.dump());
        }
    } catch (...) {
        wprintf(L"browser bridge: ignored invalid extension message\n");
    }
}

void clientThread(std::shared_ptr<BrowserClient> client) {
    char buffer[4096]{};
    int received = recv(client->socket, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        closesocket(client->socket);
        return;
    }

    std::string request(buffer, buffer + received);
    std::string accept = websocketAccept(websocketKey(request));
    if (accept.empty()) {
        closesocket(client->socket);
        return;
    }

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

    if (!sendAll(client->socket, response.data(), static_cast<int>(response.size()))) {
        closesocket(client->socket);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients.push_back(client);
    }
    wprintf(L"browser bridge: extension connected\n");
    sendRequestTargets();

    while (g_running) {
        std::string text;
        if (!readTextFrame(client->socket, &text)) break;
        if (!text.empty()) handleMessage(client, text);
    }

    removeClient(client);
    closesocket(client->socket);
    wprintf(L"browser bridge: extension disconnected\n");
}

void acceptLoop() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        wprintf(L"browser bridge: WSAStartup failed\n");
        return;
    }

    g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSocket == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int reuse = 1;
    setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (bind(g_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR
        || listen(g_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        wprintf(L"browser bridge: could not listen on 127.0.0.1:%u\n", g_port);
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    wprintf(L"browser bridge: ws://127.0.0.1:%u/browser-media\n", g_port);
    while (g_running) {
        SOCKET socket = accept(g_listenSocket, nullptr, nullptr);
        if (socket == INVALID_SOCKET) break;

        auto client = std::make_shared<BrowserClient>();
        client->socket = socket;
        std::thread(clientThread, client).detach();
    }

    if (g_listenSocket != INVALID_SOCKET) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }
    WSACleanup();
}

const char* commandName(MediaCommand command) {
    switch (command) {
    case MediaCommand::TogglePlayPause:
        return "play_pause";
    case MediaCommand::Next:
        return "next";
    case MediaCommand::Previous:
        return "previous";
    default:
        return "unknown";
    }
}

BridgeResult sendBridgeRequest(const std::string& type, const json& payload, int timeoutMs = 1800) {
    BridgeResult result;
    std::vector<std::shared_ptr<BrowserClient>> clients = clientsSnapshot();
    if (clients.empty()) return result;

    const std::string requestId = nextMessageId();
    auto pending = std::make_shared<PendingResponse>();
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingResponses[requestId] = pending;
    }

    json request;
    request["id"] = requestId;
    request["type"] = type;
    request["payload"] = payload;

    std::string text = request.dump();
    for (const auto& client : clients) {
        if (sendTextFrame(client, text)) result.sent = true;
    }

    if (!result.sent) {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingResponses.erase(requestId);
        return result;
    }

    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        result.gotResponse = pending->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return pending->done;
        });
        result.ok = pending->ok;
        result.detail = pending->message;
    }

    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingResponses.erase(requestId);
    }

    return result;
}

bool sendTrustedMediaKey(MediaCommand command) {
    WORD key = 0;
    switch (command) {
    case MediaCommand::Next:
        key = VK_MEDIA_NEXT_TRACK;
        break;
    case MediaCommand::Previous:
        key = VK_MEDIA_PREV_TRACK;
        break;
    case MediaCommand::TogglePlayPause:
        key = VK_MEDIA_PLAY_PAUSE;
        break;
    default:
        return false;
    }

    suppressNextSyntheticHotkey(key, 1200);

    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = key;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, inputs, sizeof(INPUT)) == 2;
}

bool activateBrowserTargetForTrustedKey(const std::wstring& targetId, std::string* detail = nullptr) {
    json payload = {
        {"targetId", wideToUtf8(targetId)},
        {"restoreAfterMs", 750}
    };
    BridgeResult result = sendBridgeRequest("activate_target", payload, 1200);
    if (detail) *detail = result.detail;
    return result.sent && result.gotResponse && result.ok;
}

} // namespace

void startBrowserMediaBridge(unsigned short port) {
    if (g_running.exchange(true)) return;
    g_port = port;
    g_acceptThread = std::thread(acceptLoop);
}

void stopBrowserMediaBridge() {
    if (!g_running.exchange(false)) return;

    if (g_listenSocket != INVALID_SOCKET) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        for (const auto& client : g_clients) {
            closesocket(client->socket);
        }
        g_clients.clear();
    }

    if (g_acceptThread.joinable()) g_acceptThread.join();
}

std::vector<MediaTarget> listBrowserMediaTargets(std::wstring* report) {
    std::vector<std::shared_ptr<BrowserClient>> clients = clientsSnapshot();
    if (clients.empty()) {
        if (report) *report = L"Browser bridge has no connected extension.";
        return {};
    }

    unsigned long long startRevision = 0;
    {
        std::lock_guard<std::mutex> lock(g_targetsMutex);
        startRevision = g_targetsRevision;
    }

    sendRequestTargets();

    std::unique_lock<std::mutex> lock(g_targetsMutex);
    g_targetsChanged.wait_for(lock, std::chrono::milliseconds(1800), [&] {
        return g_targetsRevision != startRevision;
    });

    if (report) {
        *report = L"Browser bridge targets: " + std::to_wstring(g_targets.size())
            + L" from " + std::to_wstring(clients.size()) + L" connected extension"
            + (clients.size() == 1 ? L"." : L"s.");
    }
    return g_targets;
}

std::vector<MediaTarget> cachedBrowserMediaTargets(std::wstring* report) {
    std::vector<std::shared_ptr<BrowserClient>> clients = clientsSnapshot();
    if (!clients.empty()) {
        sendRequestTargets();
    }

    std::lock_guard<std::mutex> lock(g_targetsMutex);
    if (report) {
        if (clients.empty()) {
            *report = L"Browser bridge has no connected extension.";
        } else {
            *report = L"Browser bridge cached targets: " + std::to_wstring(g_targets.size())
                + L" from " + std::to_wstring(clients.size()) + L" connected extension"
                + (clients.size() == 1 ? L"." : L"s.");
        }
    }
    return g_targets;
}

bool browserMediaBridgeHasClients() {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    return !g_clients.empty();
}

bool controlBrowserMediaTarget(const std::wstring& targetId, MediaCommand command, std::wstring* report) {
    if (!browserMediaBridgeHasClients()) {
        if (report) *report = L"Browser bridge has no connected extension.";
        return false;
    }

    json payload = {
        {"targetId", wideToUtf8(targetId)},
        {"command", commandName(command)}
    };

    BridgeResult domResult = sendBridgeRequest("control_media", payload);
    bool ok = domResult.sent && domResult.gotResponse && domResult.ok;

    if (report) {
        std::wstringstream out;
        if (!domResult.sent) {
            out << L"Browser bridge could not send the media command.";
        } else if (!domResult.gotResponse) {
            out << L"Browser bridge did not receive a control response.";
        } else if (ok) {
            const bool usedYoutubeHistory = domResult.detail.find("youtube:video-history") != std::string::npos;
            out << (usedYoutubeHistory ? L"[2] " : L"[1] ");
            out << L"Browser media controlled";
            if (usedYoutubeHistory) {
                out << L" via YouTube video history";
            }
            out << L".";
        } else {
            out << L"Browser bridge control failed";
            if (!domResult.detail.empty()) out << L": " << utf8ToWide(domResult.detail);
        }
        *report = out.str();
    }
    return ok;
}

std::wstring describeBrowserBridgeStatus() {
    std::lock_guard<std::mutex> clientsLock(g_clientsMutex);
    std::lock_guard<std::mutex> targetsLock(g_targetsMutex);

    std::wstringstream out;
    out << L"browser bridge clients: " << g_clients.size() << L"\n";
    out << L"browser targets: " << g_targets.size() << L"\n";
    return out.str();
}
