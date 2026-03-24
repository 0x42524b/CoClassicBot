#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <Ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr const char* GAME_EXE = "ImConquer.exe";
static constexpr const char* GAME_DIR = R"(C:\Program Files\Classic Conquer 2.0)";
static constexpr const char* GAME_PATH = R"(C:\Program Files\Classic Conquer 2.0\bin\64\ImConquer.exe)";
static constexpr const char* DLL_NAME = "coclassic.dll";
static constexpr const char* SERVER_CONFIG_NAME = "servers.json";
static constexpr const char* LOCAL_RELAY_HOST = "127.0.0.1";
static constexpr const char* RELAY_LOG_NAME = "relay_packets.log";

struct Endpoint
{
    std::string host;
    uint16_t port = 0;
};

struct LaunchOptions
{
    std::optional<Endpoint> m_proxy;
    std::optional<Endpoint> m_targetOverride;
    std::string m_proxyUser;
    std::string m_proxyPassword;
    uint16_t m_relayPort = 0;
    bool m_showHelp = false;
};

struct ManagedSocket
{
    explicit ManagedSocket(SOCKET value) : m_socket(value) {}

    SOCKET m_socket = INVALID_SOCKET;
    std::mutex m_mutex;
};

using ManagedSocketPtr = std::shared_ptr<ManagedSocket>;

static std::string EndpointToString(const Endpoint& endpoint)
{
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

static void PrintUsage()
{
    printf("Usage:\n");
    printf("  injector.exe\n");
    printf("  injector.exe --proxy <host:port> [--proxy-user <user>] [--proxy-pass <pass>]\n");
    printf("              [--relay-port <port>] [--target <host:port>]\n\n");
    printf("Examples:\n");
    printf("  injector.exe --proxy 127.0.0.1:1080\n");
    printf("  injector.exe --proxy my-socks.example:1080 --proxy-user alice --proxy-pass secret\n");
    printf("  injector.exe --proxy 10.0.0.5:1080 --relay-port 19959\n");
    printf("  injector.exe\n\n");
    printf("Notes:\n");
    printf("  - Proxy mode temporarily rewrites %s to %s:<relay-port> while the launched game is running.\n",
           SERVER_CONFIG_NAME, LOCAL_RELAY_HOST);
    printf("  - The injector stays open in proxy mode to keep the local relay alive and restores %s on exit.\n",
           SERVER_CONFIG_NAME);
    printf("  - Proxy mode logs outbound client TCP chunks to %s next to injector.exe.\n", RELAY_LOG_NAME);
    printf("  - Without --proxy, injector behavior stays close to the original launch-and-inject flow.\n");
}

static bool ParseUInt16(const std::string& text, uint16_t* value)
{
    if (!value || text.empty())
        return false;

    char* end = nullptr;
    unsigned long parsed = strtoul(text.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed > 65535)
        return false;

    *value = static_cast<uint16_t>(parsed);
    return true;
}

static bool ParseEndpoint(const std::string& text, Endpoint* endpoint)
{
    if (!endpoint || text.empty())
        return false;

    std::string host;
    std::string portText;

    if (text.front() == '[') {
        size_t close = text.find(']');
        if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':')
            return false;
        host = text.substr(1, close - 1);
        portText = text.substr(close + 2);
    } else {
        size_t colon = text.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
            return false;
        host = text.substr(0, colon);
        portText = text.substr(colon + 1);
    }

    uint16_t port = 0;
    if (host.empty() || !ParseUInt16(portText, &port))
        return false;

    endpoint->host = std::move(host);
    endpoint->port = port;
    return true;
}

static bool ParseArgs(int argc, char** argv, LaunchOptions* options)
{
    if (!options)
        return false;

    auto requireValue = [&](int index, const char* flag) -> const char* {
        if (index + 1 >= argc) {
            printf("[!] Missing value for %s\n", flag);
            return nullptr;
        }
        return argv[index + 1];
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            options->m_showHelp = true;
        } else if (arg == "--proxy") {
            const char* value = requireValue(i, "--proxy");
            if (!value)
                return false;

            Endpoint endpoint;
            if (!ParseEndpoint(value, &endpoint)) {
                printf("[!] Invalid --proxy value: %s\n", value);
                return false;
            }

            options->m_proxy = std::move(endpoint);
            ++i;
        } else if (arg == "--proxy-user") {
            const char* value = requireValue(i, "--proxy-user");
            if (!value)
                return false;
            options->m_proxyUser = value;
            ++i;
        } else if (arg == "--proxy-pass") {
            const char* value = requireValue(i, "--proxy-pass");
            if (!value)
                return false;
            options->m_proxyPassword = value;
            ++i;
        } else if (arg == "--relay-port") {
            const char* value = requireValue(i, "--relay-port");
            if (!value)
                return false;

            if (!ParseUInt16(value, &options->m_relayPort)) {
                printf("[!] Invalid --relay-port value: %s\n", value);
                return false;
            }
            ++i;
        } else if (arg == "--target") {
            const char* value = requireValue(i, "--target");
            if (!value)
                return false;

            Endpoint endpoint;
            if (!ParseEndpoint(value, &endpoint)) {
                printf("[!] Invalid --target value: %s\n", value);
                return false;
            }

            options->m_targetOverride = std::move(endpoint);
            ++i;
        } else {
            printf("[!] Unknown argument: %s\n", arg.c_str());
            return false;
        }
    }

    if (!options->m_proxy && (!options->m_proxyUser.empty() || !options->m_proxyPassword.empty() ||
                              options->m_relayPort != 0 || options->m_targetOverride.has_value())) {
        printf("[!] --proxy-user, --proxy-pass, --relay-port, and --target require --proxy.\n");
        return false;
    }

    return true;
}

static bool ReadTextFile(const fs::path& path, std::string* text)
{
    if (!text)
        return false;

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;

    text->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

static bool WriteTextFile(const fs::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;

    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

class RelayLogger
{
public:
    bool Start(fs::path path)
    {
        std::lock_guard lock(m_mutex);

        m_stream.open(path, std::ios::out | std::ios::trunc);
        if (!m_stream)
            return false;

        m_path = std::move(path);
        return true;
    }

    void Stop()
    {
        std::lock_guard lock(m_mutex);

        if (m_stream.is_open()) {
            m_stream.flush();
            m_stream.close();
        }
    }

    const fs::path& Path() const
    {
        return m_path;
    }

    void LogEvent(uint64_t connectionId, const std::string& text)
    {
        std::ostringstream line;
        line << FormatPrefix(connectionId) << text << "\n";
        WriteLocked(line.str());
    }

    void LogChunk(uint64_t connectionId, const char* direction, const uint8_t* data, size_t size)
    {
        if (!data || size == 0)
            return;

        std::ostringstream entry;
        entry << FormatPrefix(connectionId) << direction << " " << size << " bytes\n";

        for (size_t offset = 0; offset < size; offset += 16) {
            entry << "  " << std::setw(6) << std::setfill('0') << std::hex << offset << "  ";

            for (size_t i = 0; i < 16; ++i) {
                if (offset + i < size) {
                    entry << std::setw(2) << std::setfill('0') << std::hex
                          << static_cast<unsigned>(data[offset + i]) << ' ';
                } else {
                    entry << "   ";
                }
            }

            entry << " ";
            for (size_t i = 0; i < 16 && offset + i < size; ++i) {
                const uint8_t byte = data[offset + i];
                entry << (byte >= 32 && byte <= 126 ? static_cast<char>(byte) : '.');
            }
            entry << "\n";
        }

        entry << "\n";
        WriteLocked(entry.str());
    }

private:
    static std::string FormatPrefix(uint64_t connectionId)
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);

        std::ostringstream prefix;
        prefix << "["
               << std::setw(4) << std::setfill('0') << time.wYear << "-"
               << std::setw(2) << std::setfill('0') << time.wMonth << "-"
               << std::setw(2) << std::setfill('0') << time.wDay << " "
               << std::setw(2) << std::setfill('0') << time.wHour << ":"
               << std::setw(2) << std::setfill('0') << time.wMinute << ":"
               << std::setw(2) << std::setfill('0') << time.wSecond << "."
               << std::setw(3) << std::setfill('0') << time.wMilliseconds << "]";

        if (connectionId != 0)
            prefix << " [conn " << connectionId << "]";

        prefix << " ";
        return prefix.str();
    }

    void WriteLocked(const std::string& text)
    {
        std::lock_guard lock(m_mutex);
        if (!m_stream.is_open())
            return;

        m_stream << text;
        m_stream.flush();
    }

    fs::path m_path;
    std::ofstream m_stream;
    mutable std::mutex m_mutex;
};

class ServerConfigPatch
{
public:
    explicit ServerConfigPatch(fs::path path)
        : m_path(std::move(path))
    {
    }

    bool Load()
    {
        std::lock_guard lock(m_mutex);

        if (!ReadTextFile(m_path, &m_originalText)) {
            printf("[!] Failed to read %s\n", m_path.string().c_str());
            return false;
        }

        json root = json::parse(m_originalText, nullptr, true, true);
        if (!root.is_array()) {
            printf("[!] %s has an unexpected format\n", m_path.string().c_str());
            return false;
        }

        m_targets.clear();
        for (const auto& group : root) {
            if (!group.is_object() || !group.contains("servers") || !group["servers"].is_array())
                continue;

            for (const auto& server : group["servers"]) {
                if (!server.is_object())
                    continue;

                std::string address = server.value("address", "");
                int port = server.value("port", 0);
                if (address.empty() || port <= 0 || port > 65535)
                    continue;

                Endpoint endpoint{address, static_cast<uint16_t>(port)};
                auto it = std::find_if(m_targets.begin(), m_targets.end(), [&](const Endpoint& existing) {
                    return existing.host == endpoint.host && existing.port == endpoint.port;
                });
                if (it == m_targets.end())
                    m_targets.push_back(std::move(endpoint));
            }
        }

        if (m_targets.empty()) {
            printf("[!] No login servers were found in %s\n", m_path.string().c_str());
            return false;
        }

        m_loaded = true;
        return true;
    }

    bool Apply(const std::string& listenHost, uint16_t listenPort)
    {
        std::lock_guard lock(m_mutex);

        if (!m_loaded) {
            printf("[!] Server config patch was not loaded before Apply().\n");
            return false;
        }

        json root = json::parse(m_originalText, nullptr, true, true);
        for (auto& group : root) {
            if (!group.is_object() || !group.contains("servers") || !group["servers"].is_array())
                continue;

            for (auto& server : group["servers"]) {
                if (!server.is_object())
                    continue;
                server["address"] = listenHost;
                server["port"] = listenPort;
            }
        }

        if (!WriteTextFile(m_path, root.dump(2))) {
            printf("[!] Failed to write patched %s\n", m_path.string().c_str());
            return false;
        }

        m_applied = true;
        return true;
    }

    void Restore()
    {
        std::lock_guard lock(m_mutex);

        if (!m_applied)
            return;

        if (!WriteTextFile(m_path, m_originalText)) {
            printf("[!] Failed to restore %s\n", m_path.string().c_str());
            return;
        }

        printf("[+] Restored %s\n", m_path.string().c_str());
        m_applied = false;
    }

    const std::vector<Endpoint>& Targets() const
    {
        return m_targets;
    }

private:
    fs::path m_path;
    std::string m_originalText;
    std::vector<Endpoint> m_targets;
    bool m_loaded = false;
    bool m_applied = false;
    mutable std::mutex m_mutex;
};

class WinsockSession
{
public:
    bool Start()
    {
        WSADATA data{};
        int err = WSAStartup(MAKEWORD(2, 2), &data);
        if (err != 0) {
            printf("[!] WSAStartup failed (%d)\n", err);
            return false;
        }

        m_started = true;
        return true;
    }

    ~WinsockSession()
    {
        if (m_started)
            WSACleanup();
    }

private:
    bool m_started = false;
};

static SOCKET GetSocketValue(const ManagedSocketPtr& socket)
{
    if (!socket)
        return INVALID_SOCKET;

    std::lock_guard lock(socket->m_mutex);
    return socket->m_socket;
}

static void CloseManagedSocket(const ManagedSocketPtr& socket)
{
    if (!socket)
        return;

    SOCKET raw = INVALID_SOCKET;
    {
        std::lock_guard lock(socket->m_mutex);
        raw = std::exchange(socket->m_socket, INVALID_SOCKET);
    }

    if (raw != INVALID_SOCKET) {
        shutdown(raw, SD_BOTH);
        closesocket(raw);
    }
}

static void ShutdownSend(const ManagedSocketPtr& socket)
{
    SOCKET raw = GetSocketValue(socket);
    if (raw != INVALID_SOCKET)
        shutdown(raw, SD_SEND);
}

static bool SendAll(SOCKET socket, const uint8_t* data, size_t size, const char* stage)
{
    size_t sent = 0;
    while (sent < size) {
        int rc = send(socket, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
        if (rc == SOCKET_ERROR) {
            printf("[proxy] send failed during %s (0x%08X)\n", stage, WSAGetLastError());
            return false;
        }
        if (rc == 0) {
            printf("[proxy] send returned 0 during %s\n", stage);
            return false;
        }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

static bool RecvExact(SOCKET socket, uint8_t* data, size_t size, const char* stage)
{
    size_t received = 0;
    while (received < size) {
        int rc = recv(socket, reinterpret_cast<char*>(data + received), static_cast<int>(size - received), 0);
        if (rc == SOCKET_ERROR) {
            printf("[proxy] recv failed during %s (0x%08X)\n", stage, WSAGetLastError());
            return false;
        }
        if (rc == 0) {
            printf("[proxy] connection closed during %s\n", stage);
            return false;
        }
        received += static_cast<size_t>(rc);
    }
    return true;
}

static const char* SocksReplyText(uint8_t reply)
{
    switch (reply) {
    case 0x00: return "succeeded";
    case 0x01: return "general failure";
    case 0x02: return "connection not allowed";
    case 0x03: return "network unreachable";
    case 0x04: return "host unreachable";
    case 0x05: return "connection refused";
    case 0x06: return "TTL expired";
    case 0x07: return "command not supported";
    case 0x08: return "address type not supported";
    default: return "unknown error";
    }
}

static bool ConnectTcp(const Endpoint& endpoint, SOCKET* outSocket)
{
    if (!outSocket)
        return false;

    *outSocket = INVALID_SOCKET;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    std::string portText = std::to_string(endpoint.port);
    int rc = getaddrinfo(endpoint.host.c_str(), portText.c_str(), &hints, &result);
    if (rc != 0) {
        printf("[proxy] getaddrinfo failed for %s (%d)\n", EndpointToString(endpoint).c_str(), rc);
        return false;
    }

    SOCKET connected = INVALID_SOCKET;
    int lastError = 0;

    for (addrinfo* it = result; it; it = it->ai_next) {
        SOCKET socket = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socket == INVALID_SOCKET) {
            lastError = WSAGetLastError();
            continue;
        }

        if (connect(socket, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            connected = socket;
            break;
        }

        lastError = WSAGetLastError();
        closesocket(socket);
    }

    freeaddrinfo(result);

    if (connected == INVALID_SOCKET) {
        printf("[proxy] connect failed for %s (0x%08X)\n", EndpointToString(endpoint).c_str(), lastError);
        return false;
    }

    *outSocket = connected;
    return true;
}

static bool PerformSocks5Handshake(SOCKET proxySocket, const Endpoint& target,
                                   const std::string& username, const std::string& password)
{
    std::vector<uint8_t> greeting;
    greeting.push_back(0x05);
    if (!username.empty() || !password.empty()) {
        greeting.push_back(0x02);
        greeting.push_back(0x00);
        greeting.push_back(0x02);
    } else {
        greeting.push_back(0x01);
        greeting.push_back(0x00);
    }

    if (!SendAll(proxySocket, greeting.data(), greeting.size(), "SOCKS5 greeting"))
        return false;

    uint8_t methodReply[2]{};
    if (!RecvExact(proxySocket, methodReply, sizeof(methodReply), "SOCKS5 method reply"))
        return false;

    if (methodReply[0] != 0x05) {
        printf("[proxy] Unexpected SOCKS version in method reply: 0x%02X\n", methodReply[0]);
        return false;
    }

    if (methodReply[1] == 0xFF) {
        printf("[proxy] SOCKS proxy rejected all auth methods\n");
        return false;
    }

    if (methodReply[1] == 0x02) {
        if (username.size() > 255 || password.size() > 255) {
            printf("[proxy] Proxy username/password must be 255 bytes or shorter\n");
            return false;
        }

        std::vector<uint8_t> auth;
        auth.push_back(0x01);
        auth.push_back(static_cast<uint8_t>(username.size()));
        auth.insert(auth.end(), username.begin(), username.end());
        auth.push_back(static_cast<uint8_t>(password.size()));
        auth.insert(auth.end(), password.begin(), password.end());

        if (!SendAll(proxySocket, auth.data(), auth.size(), "SOCKS5 auth request"))
            return false;

        uint8_t authReply[2]{};
        if (!RecvExact(proxySocket, authReply, sizeof(authReply), "SOCKS5 auth reply"))
            return false;

        if (authReply[1] != 0x00) {
            printf("[proxy] SOCKS proxy rejected the supplied credentials\n");
            return false;
        }
    } else if (methodReply[1] != 0x00) {
        printf("[proxy] SOCKS proxy selected unsupported auth method 0x%02X\n", methodReply[1]);
        return false;
    }

    std::vector<uint8_t> request;
    request.push_back(0x05);
    request.push_back(0x01);
    request.push_back(0x00);

    IN_ADDR ipv4{};
    IN6_ADDR ipv6{};
    if (InetPtonA(AF_INET, target.host.c_str(), &ipv4) == 1) {
        request.push_back(0x01);
        auto* bytes = reinterpret_cast<const uint8_t*>(&ipv4);
        request.insert(request.end(), bytes, bytes + sizeof(ipv4));
    } else if (InetPtonA(AF_INET6, target.host.c_str(), &ipv6) == 1) {
        request.push_back(0x04);
        auto* bytes = reinterpret_cast<const uint8_t*>(&ipv6);
        request.insert(request.end(), bytes, bytes + sizeof(ipv6));
    } else {
        if (target.host.size() > 255) {
            printf("[proxy] Target host is too long for SOCKS5 domain encoding\n");
            return false;
        }

        request.push_back(0x03);
        request.push_back(static_cast<uint8_t>(target.host.size()));
        request.insert(request.end(), target.host.begin(), target.host.end());
    }

    request.push_back(static_cast<uint8_t>((target.port >> 8) & 0xFF));
    request.push_back(static_cast<uint8_t>(target.port & 0xFF));

    if (!SendAll(proxySocket, request.data(), request.size(), "SOCKS5 connect request"))
        return false;

    uint8_t replyHead[4]{};
    if (!RecvExact(proxySocket, replyHead, sizeof(replyHead), "SOCKS5 connect reply"))
        return false;

    if (replyHead[0] != 0x05) {
        printf("[proxy] Unexpected SOCKS version in connect reply: 0x%02X\n", replyHead[0]);
        return false;
    }

    if (replyHead[1] != 0x00) {
        printf("[proxy] SOCKS connect failed: %s (0x%02X)\n", SocksReplyText(replyHead[1]), replyHead[1]);
        return false;
    }

    size_t addressBytes = 0;
    if (replyHead[3] == 0x01) {
        addressBytes = 4;
    } else if (replyHead[3] == 0x04) {
        addressBytes = 16;
    } else if (replyHead[3] == 0x03) {
        uint8_t domainLength = 0;
        if (!RecvExact(proxySocket, &domainLength, sizeof(domainLength), "SOCKS5 bound domain length"))
            return false;
        addressBytes = domainLength;
    } else {
        printf("[proxy] SOCKS connect reply used unsupported address type 0x%02X\n", replyHead[3]);
        return false;
    }

    std::vector<uint8_t> trailing(addressBytes + 2);
    if (!RecvExact(proxySocket, trailing.data(), trailing.size(), "SOCKS5 bound address"))
        return false;

    return true;
}

class Socks5Relay
{
public:
    bool Start(const Endpoint& listen, const Endpoint& proxy, const Endpoint& target,
               std::string proxyUser, std::string proxyPassword, RelayLogger* logger)
    {
        m_listen = listen;
        m_proxy = proxy;
        m_target = target;
        m_proxyUser = std::move(proxyUser);
        m_proxyPassword = std::move(proxyPassword);
        m_logger = logger;

        m_listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSocket == INVALID_SOCKET) {
            printf("[proxy] Failed to create listen socket (0x%08X)\n", WSAGetLastError());
            return false;
        }

        BOOL exclusive = TRUE;
        setsockopt(m_listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(m_listen.port);
        if (InetPtonA(AF_INET, m_listen.host.c_str(), &address.sin_addr) != 1) {
            printf("[proxy] Invalid listen host: %s\n", m_listen.host.c_str());
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
            return false;
        }

        if (bind(m_listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            int bindErr = WSAGetLastError();
            if (bindErr != WSAEADDRINUSE && bindErr != WSAEACCES) {
                printf("[proxy] bind failed for %s (0x%08X)\n", EndpointToString(m_listen).c_str(), bindErr);
                closesocket(m_listenSocket);
                m_listenSocket = INVALID_SOCKET;
                return false;
            }

            printf("[proxy] Port %d in use, trying dynamic port...\n", m_listen.port);
            closesocket(m_listenSocket);

            m_listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_listenSocket == INVALID_SOCKET) {
                printf("[proxy] Failed to create retry socket (0x%08X)\n", WSAGetLastError());
                return false;
            }

            setsockopt(m_listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

            address.sin_port = htons(0);
            if (bind(m_listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
                printf("[proxy] bind failed on dynamic port (0x%08X)\n", WSAGetLastError());
                closesocket(m_listenSocket);
                m_listenSocket = INVALID_SOCKET;
                return false;
            }
        }

        sockaddr_in bound{};
        int boundLen = sizeof(bound);
        if (getsockname(m_listenSocket, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
            m_listen.port = ntohs(bound.sin_port);
        }

        if (::listen(m_listenSocket, SOMAXCONN) != 0) {
            printf("[proxy] listen failed for %s (0x%08X)\n", EndpointToString(m_listen).c_str(), WSAGetLastError());
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
            return false;
        }

        m_running = true;
        m_acceptThread = std::thread([this]() { AcceptLoop(); });

        printf("[proxy] Relay listening on %s -> %s via SOCKS5 %s\n",
               EndpointToString(m_listen).c_str(),
               EndpointToString(m_target).c_str(),
               EndpointToString(m_proxy).c_str());
        if (m_logger)
            m_logger->LogEvent(0, "Relay listening on " + EndpointToString(m_listen) +
                                      " -> " + EndpointToString(m_target) +
                                      " via SOCKS5 " + EndpointToString(m_proxy));
        return true;
    }

    void Stop()
    {
        if (!m_running.exchange(false))
            return;

        if (m_listenSocket != INVALID_SOCKET) {
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
        }

        if (m_acceptThread.joinable())
            m_acceptThread.join();

        std::vector<ManagedSocketPtr> sockets;
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(m_stateMutex);
            sockets = m_activeSockets;
            threads.swap(m_sessionThreads);
        }

        for (const auto& socket : sockets)
            CloseManagedSocket(socket);

        for (auto& thread : threads) {
            if (thread.joinable())
                thread.join();
        }

        {
            std::lock_guard lock(m_stateMutex);
            m_activeSockets.clear();
        }

        printf("[proxy] Relay stopped\n");
        if (m_logger)
            m_logger->LogEvent(0, "Relay stopped");
    }

    ~Socks5Relay()
    {
        Stop();
    }

    uint16_t GetListenPort() const { return m_listen.port; }

private:
    void AcceptLoop()
    {
        while (m_running) {
            SOCKET client = accept(m_listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (!m_running)
                    break;

                int err = WSAGetLastError();
                if (err == WSAENOTSOCK || err == WSAEINVAL)
                    break;

                printf("[proxy] accept failed (0x%08X)\n", err);
                continue;
            }

            std::lock_guard lock(m_stateMutex);
            m_sessionThreads.emplace_back([this, client]() { HandleClient(client); });
        }
    }

    void RegisterSocket(const ManagedSocketPtr& socket)
    {
        std::lock_guard lock(m_stateMutex);
        m_activeSockets.push_back(socket);
    }

    void UnregisterSocket(const ManagedSocketPtr& socket)
    {
        std::lock_guard lock(m_stateMutex);
        std::erase(m_activeSockets, socket);
    }

    static void PumpTraffic(const ManagedSocketPtr& source, const ManagedSocketPtr& destination,
                            RelayLogger* logger, uint64_t connectionId, const char* direction)
    {
        char buffer[8192];
        for (;;) {
            SOCKET sourceSocket = GetSocketValue(source);
            SOCKET destinationSocket = GetSocketValue(destination);
            if (sourceSocket == INVALID_SOCKET || destinationSocket == INVALID_SOCKET)
                break;

            int received = recv(sourceSocket, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0)
                break;

            if (logger) {
                logger->LogChunk(connectionId, direction,
                                 reinterpret_cast<const uint8_t*>(buffer),
                                 static_cast<size_t>(received));
            }

            int sent = 0;
            while (sent < received) {
                int rc = send(destinationSocket, buffer + sent, received - sent, 0);
                if (rc <= 0)
                    goto done;
                sent += rc;
            }
        }

    done:
        ShutdownSend(destination);
    }

    void HandleClient(SOCKET clientSocket)
    {
        const uint64_t connectionId = m_nextConnectionId.fetch_add(1) + 1;
        ManagedSocketPtr client = std::make_shared<ManagedSocket>(clientSocket);
        RegisterSocket(client);
        if (m_logger)
            m_logger->LogEvent(connectionId, "Accepted client connection");

        SOCKET upstreamSocket = INVALID_SOCKET;
        if (!ConnectTcp(m_proxy, &upstreamSocket)) {
            if (m_logger)
                m_logger->LogEvent(connectionId, "Failed to connect to SOCKS5 proxy");
            CloseManagedSocket(client);
            UnregisterSocket(client);
            return;
        }

        ManagedSocketPtr upstream = std::make_shared<ManagedSocket>(upstreamSocket);
        RegisterSocket(upstream);

        if (!PerformSocks5Handshake(upstreamSocket, m_target, m_proxyUser, m_proxyPassword)) {
            if (m_logger)
                m_logger->LogEvent(connectionId, "SOCKS5 handshake failed");
            CloseManagedSocket(client);
            CloseManagedSocket(upstream);
            UnregisterSocket(client);
            UnregisterSocket(upstream);
            return;
        }

        if (m_logger)
            m_logger->LogEvent(connectionId, "SOCKS5 tunnel established");

        std::thread forward(PumpTraffic, client, upstream, m_logger, connectionId, "client->target");
        std::thread backward(PumpTraffic, upstream, client, nullptr, connectionId, "target->client");

        forward.join();
        backward.join();

        if (m_logger)
            m_logger->LogEvent(connectionId, "Connection closed");

        CloseManagedSocket(client);
        CloseManagedSocket(upstream);
        UnregisterSocket(client);
        UnregisterSocket(upstream);
    }

    Endpoint m_listen;
    Endpoint m_proxy;
    Endpoint m_target;
    std::string m_proxyUser;
    std::string m_proxyPassword;

    SOCKET m_listenSocket = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_nextConnectionId{0};
    std::thread m_acceptThread;
    RelayLogger* m_logger = nullptr;

    std::mutex m_stateMutex;
    std::vector<ManagedSocketPtr> m_activeSockets;
    std::vector<std::thread> m_sessionThreads;
};

struct RuntimeContext
{
    ServerConfigPatch* m_patch = nullptr;
    Socks5Relay* m_relay = nullptr;
    RelayLogger* m_logger = nullptr;
};

static RuntimeContext* g_runtimeContext = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_runtimeContext) {
            if (g_runtimeContext->m_relay)
                g_runtimeContext->m_relay->Stop();
            if (g_runtimeContext->m_patch)
                g_runtimeContext->m_patch->Restore();
            if (g_runtimeContext->m_logger)
                g_runtimeContext->m_logger->Stop();
        }
        return FALSE;
    default:
        return FALSE;
    }
}

static bool Inject(DWORD pid, const char* dllPath)
{
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        printf("[!] OpenProcess failed (0x%08lX). Are you running as admin?\n", GetLastError());
        return false;
    }

    size_t pathLen = strlen(dllPath) + 1;
    void* remoteBuf = VirtualAllocEx(hProc, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        printf("[!] VirtualAllocEx failed (0x%08lX)\n", GetLastError());
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteBuf, dllPath, pathLen, nullptr)) {
        printf("[!] WriteProcessMemory failed (0x%08lX)\n", GetLastError());
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLibAddr, remoteBuf, 0, nullptr);
    if (!hThread) {
        printf("[!] CreateRemoteThread failed (0x%08lX)\n", GetLastError());
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (exitCode == 0) {
        printf("[!] LoadLibraryA returned NULL - injection failed.\n");
        return false;
    }

    return true;
}

static std::optional<Endpoint> SelectTargetEndpoint(const ServerConfigPatch& patch, const LaunchOptions& options)
{
    if (options.m_targetOverride)
        return options.m_targetOverride;

    const auto& targets = patch.Targets();
    if (targets.size() != 1) {
        printf("[!] %s contains multiple unique login targets.\n", SERVER_CONFIG_NAME);
        printf("    Re-run with --target <host:port> to choose which upstream target the relay should use.\n");
        return std::nullopt;
    }

    return targets.front();
}

int main(int argc, char** argv)
{
    printf("=== coclassic injector ===\n\n");

    LaunchOptions options;
    if (!ParseArgs(argc, argv, &options)) {
        PrintUsage();
        system("pause");
        return 1;
    }

    if (options.m_showHelp) {
        PrintUsage();
        return 0;
    }

    const bool proxyMode = options.m_proxy.has_value();

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    fs::path dllPath = fs::path(exePath).parent_path() / DLL_NAME;

    if (!fs::exists(dllPath)) {
        printf("[!] DLL not found: %s\n", dllPath.string().c_str());
        printf("    Build the coclassic project first.\n");
        system("pause");
        return 1;
    }

    printf("[+] DLL: %s\n", dllPath.string().c_str());

    if (!fs::exists(GAME_PATH)) {
        printf("[!] Game not found at: %s\n", GAME_PATH);
        system("pause");
        return 1;
    }

    WinsockSession winsock;
    ServerConfigPatch serverPatch(fs::path(GAME_DIR) / SERVER_CONFIG_NAME);
    Socks5Relay relay;
    RelayLogger relayLogger;
    RuntimeContext runtimeContext;

    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
        printf("[!] Failed to install console control handler (0x%08lX)\n", GetLastError());

    if (proxyMode) {
        if (!winsock.Start()) {
            system("pause");
            return 1;
        }

        fs::path logPath = fs::path(exePath).parent_path() / RELAY_LOG_NAME;
        if (!relayLogger.Start(logPath)) {
            printf("[!] Failed to open relay log: %s\n", logPath.string().c_str());
            system("pause");
            return 1;
        }

        if (!serverPatch.Load()) {
            relayLogger.Stop();
            system("pause");
            return 1;
        }

        std::optional<Endpoint> target = SelectTargetEndpoint(serverPatch, options);
        if (!target) {
            relayLogger.Stop();
            system("pause");
            return 1;
        }

        Endpoint listen{LOCAL_RELAY_HOST, options.m_relayPort != 0 ? options.m_relayPort : target->port};

        if (!relay.Start(listen, *options.m_proxy, *target,
                         options.m_proxyUser, options.m_proxyPassword, &relayLogger)) {
            relayLogger.Stop();
            system("pause");
            return 1;
        }

        if (!serverPatch.Apply(listen.host, relay.GetListenPort())) {
            relay.Stop();
            relayLogger.Stop();
            system("pause");
            return 1;
        }

        runtimeContext.m_patch = &serverPatch;
        runtimeContext.m_relay = &relay;
        runtimeContext.m_logger = &relayLogger;
        g_runtimeContext = &runtimeContext;

        printf("[proxy] Patched %s to %s:%d\n", SERVER_CONFIG_NAME, listen.host.c_str(), relay.GetListenPort());
        printf("[proxy] Upstream target: %s\n", EndpointToString(*target).c_str());
        printf("[proxy] Logging outbound client TCP chunks to %s\n", relayLogger.Path().string().c_str());
    }

    printf("[*] Launching fresh %s process...\n", GAME_EXE);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(GAME_PATH, nullptr, nullptr, nullptr, FALSE, 0,
                        nullptr, GAME_DIR, &si, &pi)) {
        printf("[!] CreateProcess failed (0x%08lX)\n", GetLastError());
        if (proxyMode) {
            relay.Stop();
            serverPatch.Restore();
            relayLogger.Stop();
        }
        system("pause");
        return 1;
    }

    const DWORD pid = pi.dwProcessId;
    printf("[+] Started %s (PID %lu)\n", GAME_EXE, pid);

    std::string dllStr = dllPath.string();
    DWORD waitResult = WaitForInputIdle(pi.hProcess, 10000);
    if (waitResult == WAIT_TIMEOUT) {
        printf("[*] WaitForInputIdle timed out, continuing with injection.\n");
    } else if (waitResult == WAIT_FAILED) {
        printf("[*] WaitForInputIdle failed (0x%08lX), continuing with injection.\n", GetLastError());
    }

    Sleep(1000);
    printf("[*] Injecting...\n");
    bool injectionSucceeded = Inject(pid, dllStr.c_str());
    if (injectionSucceeded) {
        printf("[+] Injection successful!\n");
    } else {
        printf("[!] Injection failed.\n");
        if (!proxyMode) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            system("pause");
            return 1;
        }

        printf("[*] Proxy mode is active, keeping the relay alive until the game exits.\n");
    }

    if (proxyMode) {
        serverPatch.Restore();
        printf("[+] %s restored. You can launch another instance now.\n", SERVER_CONFIG_NAME);

        printf("[*] Waiting for the game process to exit...\n");
        WaitForSingleObject(pi.hProcess, INFINITE);

        relay.Stop();
        relayLogger.Stop();
        g_runtimeContext = nullptr;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!proxyMode)
        Sleep(1000);

    return injectionSucceeded ? 0 : 1;
}
