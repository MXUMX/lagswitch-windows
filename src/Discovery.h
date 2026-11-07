#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include "Core.h"
#include "WindowsFilter.h"
#include <map>

namespace lag {
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
inline bool gameName(const wchar_t* name) {
    return _wcsicmp(name, L"GenshinImpact.exe") == 0 || _wcsicmp(name, L"YuanShen.exe") == 0;
}
inline std::uint64_t processIdentity(DWORD pid) {
    Handle h{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!h.value) throw std::runtime_error("Cannot inspect Genshin. Start LagSwitch as administrator and retry.");
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    if (!QueryFullProcessImageNameW(h.value, 0, path, &length)) throw std::runtime_error("Cannot verify the Genshin process.");
    const auto* name = wcsrchr(path, L'\\');
    if (!gameName(name ? name + 1 : path)) throw std::runtime_error("The game process changed during discovery.");
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(h.value, &created, &exited, &kernel, &user)) throw std::runtime_error("Cannot verify the game process lifetime.");
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
inline std::set<Binding> discover() {
    DWORD session{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) throw std::runtime_error("Cannot read the current Windows session.");
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot list running processes.");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::map<DWORD, std::uint64_t> processes;
    if (!Process32FirstW(snapshot.value, &entry)) throw std::runtime_error("Cannot inspect running processes.");
    do {
        DWORD candidateSession{};
        if (gameName(entry.szExeFile) && ProcessIdToSessionId(entry.th32ProcessID, &candidateSession) && candidateSession == session)
            processes.emplace(entry.th32ProcessID, processIdentity(entry.th32ProcessID));
    } while (Process32NextW(snapshot.value, &entry));
    if (processes.empty()) throw std::runtime_error("Start GenshinImpact.exe or YuanShen.exe and enter the game to prepare UDP filtering.");
    std::set<std::uint16_t> localPorts;
    for (bool v6 : {false, true}) {
        const ULONG family = v6 ? AF_INET6 : AF_INET;
        ULONG bytes{};
        auto code = GetExtendedUdpTable(nullptr, &bytes, FALSE, family, UDP_TABLE_OWNER_PID, 0);
        if (code != ERROR_INSUFFICIENT_BUFFER && code != NO_ERROR) winCheck(code, "Read UDP table size");
        if (!bytes) continue;
        std::vector<unsigned char> storage;
        // Socket tables can grow between the size query and the actual read.
        for (int attempt = 0; attempt < 4; ++attempt) {
            storage.resize(bytes);
            code = GetExtendedUdpTable(storage.data(), &bytes, FALSE, family, UDP_TABLE_OWNER_PID, 0);
            if (code != ERROR_INSUFFICIENT_BUFFER) break;
        }
        winCheck(code, "Read UDP table");
        if (v6) {
            const auto* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(storage.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) if (processes.count(table->table[i].dwOwningPid)) {
                const auto port = ntohs(static_cast<u_short>(table->table[i].dwLocalPort));
                if (port) localPorts.insert(port);
            }
        } else {
            const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(storage.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) if (processes.count(table->table[i].dwOwningPid)) {
                const auto port = ntohs(static_cast<u_short>(table->table[i].dwLocalPort));
                if (port) localPorts.insert(port);
            }
        }
    }
    // Refuse recycled PIDs and processes replaced while tables were read.
    for (const auto& [pid, identity] : processes) if (processIdentity(pid) != identity)
        throw std::runtime_error("Genshin restarted during discovery. Waiting for the next scan.");
    if (localPorts.empty()) throw std::runtime_error("Genshin has no bound UDP sockets yet. Enter the game and wait for READY.");
    std::set<Binding> ports;
    // A dual-stack IPv6 socket can also send IPv4 packets. The public UDP
    // table doesn't expose IPV6_V6ONLY, so cover both families for each port.
    for (auto port : localPorts) { ports.insert({false, port}); ports.insert({true, port}); }
    return ports;
}
} // namespace lag
