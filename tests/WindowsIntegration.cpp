#include "WindowsFilter.h"
#include <ws2tcpip.h>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
using namespace lag;
namespace {
int checks{};
void check(bool okay, const char* name) { ++checks; if (!okay) throw std::runtime_error(name); }
struct Udp {
    SOCKET handle{INVALID_SOCKET};
    sockaddr_storage address{};
    int size{};
    std::uint16_t port{};
    explicit Udp(bool v6) {
        handle = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle == INVALID_SOCKET) throw std::runtime_error("Cannot create test UDP socket");
        DWORD timeout = 200;
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (v6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&address);
            a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback; size = sizeof(*a);
        } else {
            auto* a = reinterpret_cast<sockaddr_in*>(&address);
            a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); size = sizeof(*a);
        }
        if (bind(handle, reinterpret_cast<sockaddr*>(&address), size) || getsockname(handle, reinterpret_cast<sockaddr*>(&address), &size))
            throw std::runtime_error("Cannot bind isolated loopback socket");
        port = ntohs(v6 ? reinterpret_cast<sockaddr_in6*>(&address)->sin6_port : reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    }
    ~Udp() { if (handle != INVALID_SOCKET) closesocket(handle); }
    void connectTo(const Udp& other) {
        if (connect(handle, reinterpret_cast<const sockaddr*>(&other.address), other.size)) throw std::runtime_error("Cannot connect test UDP sockets");
    }
    void drain() {
        u_long nonblocking = 1; ioctlsocket(handle, FIONBIO, &nonblocking);
        char data[64]; while (recv(handle, data, sizeof(data), 0) >= 0) {}
        nonblocking = 0; ioctlsocket(handle, FIONBIO, &nonblocking);
    }
};
bool delivered(Udp& source, Udp& destination) {
    destination.drain();
    const char marker = 'L';
    // A block may cause send() to fail or drop after send(), depending on layer.
    send(source.handle, &marker, 1, 0);
    char received{};
    return recv(destination.handle, &received, 1, 0) == 1 && received == marker;
}
std::vector<Rule> probeRules(bool v6, std::uint16_t port) {
    return {{v6, false, false, port, true}, {v6, true, false, port, true}};
}
void familyTest(bool v6) {
    Udp game(v6), server(v6), unrelated(v6), unrelatedServer(v6);
    game.connectTo(server); server.connectTo(game);
    unrelated.connectTo(unrelatedServer); unrelatedServer.connectTo(unrelated);
    check(delivered(game, server) && delivered(server, game), "Established UDP loopback flow before block");
    {
        WindowsFilter filter;
        filter.block(probeRules(v6, game.port));
        // Inbound and outbound on an already-established connected UDP socket.
        check(!delivered(game, server), "Existing outbound UDP packets are blocked");
        check(!delivered(server, game), "Existing inbound UDP packets are blocked");
        check(delivered(unrelated, unrelatedServer), "Unrelated UDP port remains available");
        filter.restore();
        check(delivered(game, server) && delivered(server, game), "Removing rules restores both directions");
        filter.block(probeRules(v6, game.port));
    }
    check(delivered(game, server), "Closing dynamic session removes filters");
    {
        WindowsFilter filter;
        auto rules = plan(Scope::Genshin, {{v6, game.port}});
        filter.block(rules);
        check(delivered(game, server) && delivered(server, game), "Production rules leave loopback online");
    }
    // A forced child-process exit must also clean up its dynamic WFP session.
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --hold " + std::to_wstring(game.port) + (v6 ? L" 6" : L" 4");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read{}, write{};
    check(CreatePipe(&read, &write, &security, 0) != FALSE, "Create child ready pipe");
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write; startup.hStdError = write; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION child{};
    const auto created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    CloseHandle(write);
    if (!created) { CloseHandle(read); throw std::runtime_error("Cannot start isolated dynamic-session child"); }
    struct Child {
        PROCESS_INFORMATION p;
        HANDLE pipe;
        ~Child() { TerminateProcess(p.hProcess, 0); WaitForSingleObject(p.hProcess, 3000); CloseHandle(p.hThread); CloseHandle(p.hProcess); CloseHandle(pipe); }
    } cleanup{child, read};
    bool ready{};
    for (int i = 0; i < 100; ++i) {
        DWORD available{};
        if (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available) {
            char message[32]{}; DWORD got{}; ReadFile(read, message, sizeof(message) - 1, &got, nullptr);
            ready = std::string(message, got).find("READY") != std::string::npos;
            break;
        }
        if (WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(ready, "Child installed isolated filters");
    check(!delivered(game, server), "Child filter active before forced exit");
    TerminateProcess(child.hProcess, 0); WaitForSingleObject(child.hProcess, 3000);
    bool restored{};
    for (int i = 0; i < 25 && !restored; ++i) { restored = delivered(game, server); if (!restored) Sleep(100); }
    check(restored, "Windows cleans dynamic filters after forced process exit");
    std::cout << (v6 ? "IPv6" : "IPv4") << " isolated UDP drop/restore/crash tests passed.\n";
}
} // namespace
int main(int argc, char** argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    try {
        if (argc == 4 && std::string(argv[1]) == "--hold") {
            const auto value = std::stoi(argv[2]);
            if (value < 1 || value > 65535) throw std::runtime_error("Invalid probe port");
            WindowsFilter filter;
            filter.block(probeRules(std::string(argv[3]) == "6", static_cast<std::uint16_t>(value)));
            std::cout << "READY\n" << std::flush;
            Sleep(15000); // Parent terminates this process after checking the filter.
        } else {
            familyTest(false); familyTest(true);
            std::cout << "PASS: " << checks << " Windows integration checks; only ephemeral loopback UDP test ports filtered.\n";
        }
        WSACleanup(); return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; WSACleanup(); return 1; }
}
