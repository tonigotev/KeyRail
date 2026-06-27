#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "discord_bridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static std::atomic<bool> g_running{false};
static SOCKET g_listenSocket = INVALID_SOCKET;
static std::thread g_acceptThread;
static std::mutex g_clientsMutex;
static std::vector<SOCKET> g_clients;
static unsigned short g_port = 8787;

static std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), size);
    out.pop_back();
    return out;
}

static std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
        value.pop_back();
    }
    while (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
    }
    return value;
}

static std::string websocketKey(const std::string& request) {
    const std::string needle = "Sec-WebSocket-Key:";
    size_t start = request.find(needle);
    if (start == std::string::npos) return {};
    start += needle.size();
    size_t end = request.find("\r\n", start);
    if (end == std::string::npos) return {};
    return trim(request.substr(start, end - start));
}

static bool sha1(const std::string& input, unsigned char out[20]) {
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

static std::string base64(const unsigned char* data, size_t size) {
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

static std::string websocketAccept(const std::string& key) {
    unsigned char digest[20]{};
    if (!sha1(key + kWebSocketGuid, digest)) return {};
    return base64(digest, sizeof(digest));
}

static bool sendAll(SOCKET socket, const char* data, int length) {
    int sent = 0;
    while (sent < length) {
        int result = send(socket, data + sent, length - sent, 0);
        if (result <= 0) return false;
        sent += result;
    }
    return true;
}

static bool sendTextFrame(SOCKET socket, const std::string& text) {
    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    if (text.size() < 126) {
        frame.push_back(static_cast<unsigned char>(text.size()));
    } else if (text.size() <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>((text.size() >> 8) & 0xff));
        frame.push_back(static_cast<unsigned char>(text.size() & 0xff));
    } else {
        return false;
    }

    frame.insert(frame.end(), text.begin(), text.end());
    return sendAll(socket, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()));
}

static void removeClient(SOCKET socket) {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), socket), g_clients.end());
}

static void clientThread(SOCKET socket) {
    char buffer[4096]{};
    int received = recv(socket, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        closesocket(socket);
        return;
    }

    std::string request(buffer, buffer + received);
    std::string accept = websocketAccept(websocketKey(request));
    if (accept.empty()) {
        closesocket(socket);
        return;
    }

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

    if (!sendAll(socket, response.data(), static_cast<int>(response.size()))) {
        closesocket(socket);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients.push_back(socket);
    }
    wprintf(L"discord bridge: Vencord client connected\n");

    while (g_running) {
        received = recv(socket, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) break;

        std::string text(buffer, buffer + received);
        wprintf(L"discord bridge ack/frame: %s\n", utf8ToWide(text).c_str());
    }

    removeClient(socket);
    closesocket(socket);
    wprintf(L"discord bridge: Vencord client disconnected\n");
}

static void acceptLoop() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        wprintf(L"discord bridge: WSAStartup failed\n");
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
        wprintf(L"discord bridge: could not listen on 127.0.0.1:%u\n", g_port);
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    wprintf(L"discord bridge: ws://127.0.0.1:%u\n", g_port);
    while (g_running) {
        SOCKET client = accept(g_listenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        std::thread(clientThread, client).detach();
    }

    if (g_listenSocket != INVALID_SOCKET) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }
    WSACleanup();
}

void startDiscordBridge(unsigned short port) {
    if (g_running.exchange(true)) return;
    g_port = port;
    g_acceptThread = std::thread(acceptLoop);
}

void stopDiscordBridge() {
    if (!g_running.exchange(false)) return;

    if (g_listenSocket != INVALID_SOCKET) {
        closesocket(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        for (SOCKET client : g_clients) {
            closesocket(client);
        }
        g_clients.clear();
    }

    if (g_acceptThread.joinable()) {
        g_acceptThread.join();
    }
}

bool sendDiscordBridgeCommand(const std::string& command, std::wstring* report) {
    std::vector<SOCKET> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        snapshot = g_clients;
    }

    if (snapshot.empty()) {
        if (report) *report = L"Discord bridge has no connected Vencord clients.";
        return false;
    }

    int sent = 0;
    for (SOCKET client : snapshot) {
        if (sendTextFrame(client, command)) ++sent;
    }

    if (report) {
        std::wstringstream text;
        text << L"Discord bridge sent '" << utf8ToWide(command) << L"' to " << sent
             << L" connected Vencord client" << (sent == 1 ? L"." : L"s.");
        *report = text.str();
    }
    return sent > 0;
}
