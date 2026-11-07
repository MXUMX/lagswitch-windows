# LagSwitch for Windows 1.0

A portable notification-area app for temporary network packet drops. Genshin UDP filtering is selected by default, with an explicit system-wide option. Uses the Windows Filtering Platform (WFP) included with Windows; no extra driver, third-party firewall, subscription, .NET runtime, or developer certificate is required.

## Run

1. Extract the Windows x64 download, then open `LagSwitch.exe` on Windows 10 or 11 (64-bit).
2. Approve the administrator/UAC prompt. Look for **LS** in the notification area; it may be under the hidden-icons arrow.
3. Launch Genshin and enter the game. Hover over LS: wait for **READY** (green). **SCAN** (amber) means no fresh game UDP ports are available.
4. Press **Ctrl+Alt+Numpad 1** to toggle drops. The icon turns red and the tooltip says **GI DROP**. Press the same shortcut again to restore traffic.

Click or right-click LS for the toggle, Settings, and **Restore Traffic and Quit**. Settings offers **Ctrl+Alt+1** or **Ctrl+Shift+1** for keyboards without a numeric keypad. Numpad 1 requires Num Lock on. A conflicting shortcut is reported; the menu remains usable. This is a standard global Windows hotkey, so it does not require injecting code into Genshin.

The executable is unsigned and has no SmartScreen reputation. Windows may show an unknown-publisher warning. No signing certificate was purchased. Use the source/Actions build and SHA-256 to identify the download; do not disable Windows security features to run it.

## Settings

- **Genshin UDP only** starts enabled. It discovers `GenshinImpact.exe` and `YuanShen.exe` in the current Windows session. Unchecking it selects system-wide dropping. Changing scope restores traffic first.
- **Automatically restore traffic** starts disabled. Enable it and enter a duration **in minutes**: `0.5` = 30 seconds, `2.5` = 2 minutes 30 seconds. Positive values up to 1440 minutes are accepted. Each activation starts a fresh interval. Editing the duration while blocked measures from the original activation; an already-expired new duration restores immediately. Disabling the timer cancels that deadline.
- Settings are saved locally under `HKEY_CURRENT_USER\Software\LagSwitchWindows`. No telemetry, game paths, credentials, or network traffic are uploaded.

The timer and a six-second UI-heartbeat watchdog run independently of the UI message loop, including while Settings is open. Traffic is restored before sleep, session lock/logoff, and normal quit. The WFP session is dynamic: Windows removes the app's temporary filters when the process exits, including a crash or forced termination. The app installs no service, startup task, or persistent firewall rule and does not disable Windows Firewall or clear anyone else's rules.

## How Genshin mode differs from the Mac version

Windows' UDP socket table provides the owning process and local port, but does not provide a connected UDP remote-server address like macOS `lsof`. This version therefore filters **UDP packets using the game's detected local ports**, in both directions, with IPv4 and IPv6 support. TCP login, launchers, browser traffic, downloads, and unrelated UDP ports remain available.

This is port-based packet filtering, not a per-packet process firewall. Another application sharing the same UDP port and IP family would also be affected. It covers all remote destinations of each captured game UDP port. The captured set stays fixed while DROP is active; a newly created local port is covered on the next activation after background discovery. If the game exits or restarts during DROP, restore traffic before reusing the toggle. Both modes leave loopback available.

Discovery checks process names, session ownership, and process creation times before and after reading socket tables to reject recycled PIDs. It runs once per second in a worker thread; results expire after five seconds. No process enumeration, DNS resolution, PowerShell command, or external firewall process runs on the hotkey path. Activation adds the cached filter plan in one WFP transaction, and restoration deletes only those filters. Rules operate at packet/transport layers so already-established UDP traffic is covered. Actual in-game latency has not been measured.

System-wide mode filters inbound/outbound IPv4 and IPv6 packets; it can interrupt VPNs, calls, downloads, and remote sessions. Non-IP Ethernet traffic such as ARP is outside its scope. Packets are dropped, not queued for later delivery. Restoring removes LagSwitch's rules; applications may still need to reconnect. Other network filters/VPNs can affect WFP behavior. The app does not modify Genshin or its anti-cheat.

## Build on Windows or GitHub Actions

The repository includes `.github/workflows/windows.yml`. Push to `main` or run **Actions → Build Windows app → Run workflow**. The Windows runner compiles using Microsoft's C++ compiler and Windows SDK, runs the tests, and publishes **LagSwitch-Windows-x64** containing the executable, this README, and a SHA-256 checksum. Artifacts are retained for 30 days. No GitHub secrets are needed.

To build locally, install Visual Studio 2022 Build Tools with **Desktop development with C++**, the Windows SDK, and CMake. In an administrator PowerShell window:

```powershell
.\Build.ps1
```

Output: `dist\LagSwitch.exe`. The C++ runtime is statically linked. `-SkipTests` builds without running tests. The core tests also compile on macOS/Linux using CMake, without Windows libraries.

## Validation

Portable core tests cover default scope, IPv4/IPv6 filter plans, cache freshness, failure handling, frozen active rules, decimal-minute input, timer changes, watchdog recovery, and concurrent discovery updates. Windows integration tests use only dynamically allocated **loopback UDP test ports**, never a system-wide test block. They establish real UDP flows, check drops in both directions, verify unrelated ports remain available, restore the same sockets, check production loopback exclusions, and force-terminate a child to verify dynamic-session cleanup. These tests require elevation and run on the Windows Actions runner. They do not require Genshin or send traffic to a game server.

The tray interface, global shortcut in Genshin, suspend/session events, VPN compatibility, and real gameplay still require hands-on Windows testing.

Implementation references: Microsoft's [WFP layer conditions](https://learn.microsoft.com/en-us/windows/win32/fwp/filtering-conditions-available-at-each-filtering-layer), [dynamic-session cleanup](https://learn.microsoft.com/en-us/windows/win32/api/fwpmu/nf-fwpmu-fwpmengineopen0), and [UDP table API](https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedudptable).
