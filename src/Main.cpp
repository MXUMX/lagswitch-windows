#include "Discovery.h"
#include "resource.h"
#include "Shortcut.h"
#include "ToggleMailbox.h"
#include <shellapi.h>
#include <wtsapi32.h>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <memory>

using namespace lag;
namespace {
constexpr UINT TrayMessage = WM_APP + 1, ErrorMessage = WM_APP + 2, OperationDone = WM_APP + 3, RecordedKey = WM_APP + 4;
constexpr UINT ToggleId = 1, SettingsId = 2, QuitId = 3, RestoreId = 4;
constexpr wchar_t RegistryKey[] = L"Software\\LagSwitchWindows";
const wchar_t* shortcuts[] = {L"Ctrl+Alt+Numpad 1", L"Ctrl+Alt+1", L"Ctrl+Shift+1", L"Custom recorded sequence"};
struct Preferences { Scope scope{Scope::Genshin}; bool timed{}; double minutes{0.5}; int shortcut{}; std::vector<Stroke> custom; };
Preferences load() {
    Preferences p;
    DWORD value{}, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, RegistryKey, L"GenshinOnly", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) p.scope = value ? Scope::Genshin : Scope::System;
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, RegistryKey, L"AutoRestore", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) p.timed = value != 0;
    size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, RegistryKey, L"Shortcut", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value < 4) p.shortcut = static_cast<int>(value);
    wchar_t sequence[256]{}; size = sizeof(sequence);
    if (RegGetValueW(HKEY_CURRENT_USER, RegistryKey, L"CustomSequence", RRF_RT_REG_SZ, nullptr, sequence, &size) == ERROR_SUCCESS) {
        try { p.custom = decodeSequence(sequence); } catch (...) {}
    }
    if (p.shortcut == 3 && !validSequence(p.custom)) p.shortcut = 0;
    wchar_t text[64]{}; size = sizeof(text);
    if (RegGetValueW(HKEY_CURRENT_USER, RegistryKey, L"Minutes", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS) {
        try { p.minutes = parseMinutes(text); } catch (...) { p.minutes = 0.5; }
    }
    return p;
}
void save(const Preferences& p) {
    HKEY key{};
    winCheck(RegCreateKeyExW(HKEY_CURRENT_USER, RegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr), "Open settings");
    const DWORD scope = p.scope == Scope::Genshin, timed = p.timed, shortcut = static_cast<DWORD>(p.shortcut);
    wchar_t minutes[64]{}; swprintf_s(minutes, L"%.9g", p.minutes);
    LSTATUS code = RegSetValueExW(key, L"GenshinOnly", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&scope), sizeof(scope));
    if (code == ERROR_SUCCESS) code = RegSetValueExW(key, L"AutoRestore", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&timed), sizeof(timed));
    if (code == ERROR_SUCCESS) code = RegSetValueExW(key, L"Shortcut", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&shortcut), sizeof(shortcut));
    if (code == ERROR_SUCCESS) code = RegSetValueExW(key, L"Minutes", 0, REG_SZ, reinterpret_cast<const BYTE*>(minutes), static_cast<DWORD>((wcslen(minutes) + 1) * sizeof(wchar_t)));
    const auto sequence = encodeSequence(p.custom);
    if (code == ERROR_SUCCESS) code = RegSetValueExW(key, L"CustomSequence", 0, REG_SZ, reinterpret_cast<const BYTE*>(sequence.c_str()), static_cast<DWORD>((sequence.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    winCheck(code, "Save settings");
}
std::wstring wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }
void showError(HWND owner, const std::exception& e) { MessageBoxW(owner, wide(e.what()).c_str(), L"LagSwitch", MB_OK | MB_ICONWARNING); }
HICON icon(COLORREF color) {
    HDC dc = GetDC(nullptr), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, 32, 32), mask = CreateBitmap(32, 32, 1, 1, nullptr);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    RECT bounds{0, 0, 32, 32};
    FillRect(memory, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(memory, brush), oldPen = SelectObject(memory, GetStockObject(NULL_PEN));
    Ellipse(memory, 0, 0, 32, 32);
    SelectObject(memory, oldPen); SelectObject(memory, oldBrush); DeleteObject(brush);
    SetBkMode(memory, TRANSPARENT); SetTextColor(memory, RGB(255, 255, 255));
    HFONT font = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto oldFont = SelectObject(memory, font);
    DrawTextW(memory, L"LS", 2, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(memory, oldFont); DeleteObject(font);
    SelectObject(memory, mask); PatBlt(memory, 0, 0, 32, 32, BLACKNESS);
    SelectObject(memory, previous);
    ICONINFO info{TRUE, 0, 0, mask, bitmap};
    HICON result = CreateIconIndirect(&info);
    DeleteObject(bitmap); DeleteObject(mask); DeleteDC(memory); ReleaseDC(nullptr, dc);
    return result;
}
class App;
App* app{};
INT_PTR CALLBACK settingsProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK recorderProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK keyboardProc(int, WPARAM, LPARAM);
LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
std::wstring sequenceLabel(const std::vector<Stroke>& strokes) {
    std::wstring result;
    for (const auto& s : strokes) {
        if (!result.empty()) result += L" then ";
        if (s.modifiers & 1) result += L"Ctrl+";
        if (s.modifiers & 2) result += L"Alt+";
        if (s.modifiers & 4) result += L"Shift+";
        if (s.modifiers & 8) result += L"Win+";
        UINT scan = MapVirtualKeyW(s.key, MAPVK_VK_TO_VSC);
        if ((s.key >= VK_PRIOR && s.key <= VK_DOWN) || s.key == VK_INSERT || s.key == VK_DELETE || s.key == VK_DIVIDE) scan |= 0x100;
        wchar_t name[80]{};
        if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, static_cast<int>(std::size(name)))) result += name;
        else result += L"Key " + std::to_wstring(s.key);
    }
    return result.empty() ? L"No sequence recorded" : result;
}
class App {
    WindowsFilter filter_;
    Controller controller_{filter_};
    Preferences prefs_{load()};
    std::thread discovery_, watchdog_, operations_;
    ToggleMailbox requests_;
    std::mutex operationGate_;
    HHOOK keyboard_{};
    bool down_[256]{};
    SequenceMatcher matcher_;
    HWND recorder_{};
    std::vector<Stroke> recorded_;
    std::atomic<bool> stop_{};
    std::mutex waitMutex_;
    std::condition_variable wait_;
    NOTIFYICONDATAW tray_{};
    HICON online_{icon(RGB(30, 135, 75))}, scanning_{icon(RGB(170, 110, 15))}, dropped_{icon(RGB(195, 45, 45))};
    std::wstring lastTip_;
    UINT taskbarRestart_{RegisterWindowMessageW(L"TaskbarCreated")};
    int hotkeyId_{0x4C53};
    bool nativeRegistered_{};
    bool added_{}, settingsOpen_{};
public:
    HWND window{};
    App() {
        controller_.configure(prefs_.scope, prefs_.timed, prefs_.minutes, GetTickCount64());
        controller_.heartbeat(GetTickCount64());
    }
    ~App() {
        stop_ = true; wait_.notify_all();
        requests_.stop();
        if (keyboard_) UnhookWindowsHookEx(keyboard_);
        if (operations_.joinable()) operations_.join();
        try { controller_.restore(); } catch (...) { filter_.close(); }
        if (watchdog_.joinable()) watchdog_.join();
        if (discovery_.joinable()) discovery_.join();
        if (window) { KillTimer(window, 1); UnregisterHotKey(window, hotkeyId_); WTSUnRegisterSessionNotification(window); }
        if (added_) Shell_NotifyIconW(NIM_DELETE, &tray_);
        for (auto image : {online_, scanning_, dropped_}) if (image) DestroyIcon(image);
        if (window && IsWindow(window)) DestroyWindow(window);
    }
    bool registerShortcut(int choice, int id) {
        UINT mods = choice == 2 ? MOD_CONTROL | MOD_SHIFT : MOD_CONTROL | MOD_ALT;
        UINT key = choice == 0 ? VK_NUMPAD1 : '1';
        return RegisterHotKey(window, id, mods | MOD_NOREPEAT, key) != FALSE;
    }
    void ensureKeyboardHook() {
        if (keyboard_) return;
        for (int i = 0; i < 256; ++i) down_[i] = (GetAsyncKeyState(i) & 0x8000) != 0;
        keyboard_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, GetModuleHandleW(nullptr), 0);
        if (!keyboard_) winCheck(GetLastError(), "Enable custom shortcut listener");
    }
    void releaseKeyboardHookIfUnused() {
        if (keyboard_ && prefs_.shortcut != 3 && !recorder_) { UnhookWindowsHookEx(keyboard_); keyboard_ = nullptr; }
    }
    bool keyboard(WPARAM message, const KBDLLHOOKSTRUCT& event) {
        if (event.vkCode > 255 || (event.flags & LLKHF_INJECTED)) return false;
        const bool capture = recorder_ && GetForegroundWindow() == recorder_;
        const auto key = static_cast<std::uint16_t>(event.vkCode);
        if (message == WM_KEYUP || message == WM_SYSKEYUP) { down_[key] = false; return capture; }
        if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return false;
        const bool repeat = down_[key]; down_[key] = true;
        if (repeat || key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_MENU || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN) return capture;
        const auto held = [&](int general, int left, int right) { return down_[general] || down_[left] || down_[right] || (GetAsyncKeyState(general) & 0x8000); };
        const std::uint16_t mods = static_cast<std::uint16_t>((held(VK_CONTROL, VK_LCONTROL, VK_RCONTROL) ? 1 : 0) | (held(VK_MENU, VK_LMENU, VK_RMENU) ? 2 : 0) | (held(VK_SHIFT, VK_LSHIFT, VK_RSHIFT) ? 4 : 0) | (down_[VK_LWIN] || down_[VK_RWIN] ? 8 : 0));
        if (recorder_) {
            if (GetForegroundWindow() == recorder_) PostMessageW(recorder_, RecordedKey, key, mods);
        } else if (!settingsOpen_ && prefs_.shortcut == 3 && matcher_.key({key, mods}, GetTickCount64())) {
            // Never perform a firewall RPC inside the keyboard hook.
            requests_.toggle();
        }
        return capture;
    }
    std::vector<Stroke> record(HWND owner) {
        ensureKeyboardHook(); recorded_.clear(); matcher_.reset();
        const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_RECORD), owner, recorderProc, 0);
        recorder_ = nullptr; releaseKeyboardHookIfUnused(); matcher_.reset();
        return result == IDOK ? recorded_ : std::vector<Stroke>{};
    }
    void recorderWindow(HWND window) { recorder_ = window; }
    void recordKey(Stroke stroke, HWND dialog) {
        if (recorded_.size() < 4) recorded_.push_back(stroke);
        SetDlgItemTextW(dialog, IDC_CAPTURE, sequenceLabel(recorded_).c_str());
    }
    void clearRecording(HWND dialog) { recorded_.clear(); SetDlgItemTextW(dialog, IDC_CAPTURE, L"Press your keys..."); }
    bool hasRecording() const { return !recorded_.empty(); }
    void start(HINSTANCE instance) {
        WNDCLASSW cls{}; cls.lpfnWndProc = windowProc; cls.hInstance = instance; cls.lpszClassName = L"LagSwitchWindow";
        if (!RegisterClassW(&cls)) winCheck(GetLastError(), "Register tray window");
        window = CreateWindowExW(0, cls.lpszClassName, L"LagSwitch", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!window) winCheck(GetLastError(), "Create tray window");
        matcher_.set(prefs_.custom);
        if (prefs_.shortcut == 3) ensureKeyboardHook();
        else if (!(nativeRegistered_ = registerShortcut(prefs_.shortcut, hotkeyId_))) {
            MessageBoxW(window, L"The saved shortcut is already in use. Use the tray menu to toggle, or choose another shortcut in Settings.", L"LagSwitch", MB_OK | MB_ICONWARNING);
        }
        tray_.cbSize = sizeof(tray_); tray_.hWnd = window; tray_.uID = 1;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; tray_.uCallbackMessage = TrayMessage;
        updateTray(true);
        if (!SetTimer(window, 1, 100, nullptr)) throw std::runtime_error("Cannot start the UI heartbeat timer.");
        WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
        operations_ = std::thread([this] {
            for (;;) {
                const auto ticket = requests_.take();
                if (ticket.request == ToggleMailbox::Request::Stop) return;
                std::lock_guard<std::mutex> gate(operationGate_);
                if (!requests_.current(ticket)) continue;
                try {
                    if (ticket.request == ToggleMailbox::Request::Restore) controller_.restore();
                    else controller_.toggle(GetTickCount64());
                    PostMessageW(window, OperationDone, 0, 0);
                } catch (const std::exception& e) {
                    requests_.clear();
                    auto error = std::make_unique<std::string>(e.what());
                    if (PostMessageW(window, ErrorMessage, 0, reinterpret_cast<LPARAM>(error.get()))) error.release();
                }
            }
        });
        discovery_ = std::thread([this] {
            while (!stop_) {
                if (controller_.status(GetTickCount64()).scope == Scope::Genshin) {
                    const auto began = GetTickCount64();
                    try { controller_.updateDiscovery(discover(), began); }
                    catch (const std::exception& e) { controller_.updateDiscovery({}, began, e.what()); }
                }
                std::unique_lock<std::mutex> lock(waitMutex_);
                wait_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_.load(); });
            }
        });
        watchdog_ = std::thread([this] {
            while (!stop_) {
                try { controller_.tick(GetTickCount64()); }
                catch (...) { PostMessageW(window, ErrorMessage, 0, 0); }
                std::unique_lock<std::mutex> lock(waitMutex_);
                wait_.wait_for(lock, std::chrono::milliseconds(50), [this] { return stop_.load(); });
            }
        });
    }
    void updateTray(bool add = false, const Status* cached = nullptr) {
        const auto state = cached ? *cached : controller_.status(GetTickCount64());
        std::wstring text = L"LagSwitch - ";
        text += state.blocked ? (state.scope == Scope::Genshin ? L"GI DROP" : L"DROP") : (state.scope == Scope::System ? L"ONLINE" : state.ready ? L"READY" : L"SCAN");
        if (state.blocked && state.timed) text += L" (" + std::to_wstring((state.remaining + 999) / 1000) + L"s remaining)";
        else if (state.scope == Scope::Genshin && state.ready) text += L" - " + std::to_wstring(state.portCount) + L" UDP port(s)";
        text += L"\n"; text += prefs_.shortcut == 3 ? sequenceLabel(prefs_.custom) : shortcuts[prefs_.shortcut];
        if (!add && lastTip_ == text) return;
        wcsncpy_s(tray_.szTip, text.c_str(), _TRUNCATE);
        tray_.hIcon = state.blocked ? dropped_ : state.ready ? online_ : scanning_;
        if (!Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &tray_)) {
            if (add) throw std::runtime_error("Cannot add the notification-area icon.");
            return;
        }
        added_ = true; lastTip_ = text;
    }
    void toggle() { if (!settingsOpen_) requests_.toggle(); }
    void restore() {
        std::lock_guard<std::mutex> gate(operationGate_);
        requests_.clear(); controller_.restore(); updateTray();
    }
    void settings() {
        if (settingsOpen_) return;
        settingsOpen_ = true;
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), window, settingsProc, 0);
        settingsOpen_ = false;
    }
    void menu() {
        HMENU menu = CreatePopupMenu();
        const auto state = controller_.status(GetTickCount64());
        std::wstring label = state.blocked ? L"Restore Traffic" : state.scope == Scope::System ? L"Drop System-wide Traffic" : L"Drop Genshin UDP Traffic";
        AppendMenuW(menu, MF_STRING, state.blocked ? RestoreId : ToggleId, label.c_str());
        AppendMenuW(menu, MF_STRING, SettingsId, L"Settings...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, QuitId, L"Restore Traffic and Quit");
        POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(window);
        const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(menu); PostMessageW(window, WM_NULL, 0, 0);
        if (selected) PostMessageW(window, WM_COMMAND, selected, 0);
    }
    Preferences preferences() const { return prefs_; }
    std::wstring detection() const {
        const auto s = controller_.status(GetTickCount64());
        if (s.scope == Scope::System) return L"System-wide mode selected.";
        return s.ready ? L"Ready: " + std::to_wstring(s.portCount) + L" Genshin UDP port(s)." : wide(s.detectionError.empty() ? "Scanning for Genshin UDP sockets..." : s.detectionError);
    }
    void apply(const Preferences& next) {
        const int newId = hotkeyId_ == 0x4C53 ? 0x4C54 : 0x4C53;
        if (next.shortcut == 3 && !validSequence(next.custom)) throw std::runtime_error("Record a custom sequence first.");
        const bool changed = next.shortcut != prefs_.shortcut || (next.shortcut != 3 && !nativeRegistered_);
        if (next.shortcut == 3) ensureKeyboardHook();
        if (changed && next.shortcut != 3 && !registerShortcut(next.shortcut, newId)) throw std::runtime_error("That shortcut is already in use. Choose another.");
        std::lock_guard<std::mutex> gate(operationGate_);
        requests_.clear();
        try { controller_.configure(next.scope, next.timed, next.minutes, GetTickCount64()); }
        catch (...) { if (changed && next.shortcut != 3) UnregisterHotKey(window, newId); throw; }
        if (changed) { UnregisterHotKey(window, hotkeyId_); hotkeyId_ = newId; nativeRegistered_ = next.shortcut != 3; }
        prefs_ = next;
        matcher_.set(prefs_.custom); releaseKeyboardHookIfUnused();
        updateTray();
        save(prefs_);
    }
    LRESULT message(UINT message, WPARAM w, LPARAM l) {
        if (message == taskbarRestart_) { updateTray(true); return 0; }
        switch (message) {
        case OperationDone:
        case WM_TIMER: { Status state; if (controller_.heartbeatAndStatus(GetTickCount64(), state)) updateTray(false, &state); return 0; }
        case WM_HOTKEY: if (static_cast<int>(w) == hotkeyId_) toggle(); return 0;
        case TrayMessage: if (l == WM_RBUTTONUP || l == WM_LBUTTONUP || l == WM_CONTEXTMENU) menu(); return 0;
        case WM_COMMAND:
            if (LOWORD(w) == ToggleId) toggle();
            if (LOWORD(w) == RestoreId) restore();
            if (LOWORD(w) == SettingsId) settings();
            if (LOWORD(w) == QuitId) { restore(); PostQuitMessage(0); }
            return 0;
        case WM_POWERBROADCAST: if (w == PBT_APMSUSPEND) restore(); return TRUE;
        case WM_WTSSESSION_CHANGE: if (w == WTS_SESSION_LOCK || w == WTS_SESSION_LOGOFF) restore(); return 0;
        case WM_QUERYENDSESSION: restore(); return TRUE;
        case WM_ENDSESSION: if (w) { restore(); PostQuitMessage(0); } return 0;
        case WM_CLOSE: restore(); PostQuitMessage(0); return 0;
        case ErrorMessage: {
            std::unique_ptr<std::string> error(reinterpret_cast<std::string*>(l));
            throw std::runtime_error(error ? *error : "The firewall command failed. The temporary filter session was closed to restore traffic.");
        }
        default: return DefWindowProcW(window, message, w, l);
        }
    }
};
LRESULT CALLBACK keyboardProc(int code, WPARAM message, LPARAM event) {
    if (code == HC_ACTION && app && app->keyboard(message, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(event))) return 1;
    return CallNextHookEx(nullptr, code, message, event);
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    // CreateWindow sends messages before App::window has been assigned.
    if (!app || !app->window) return DefWindowProcW(window, message, w, l);
    try { return app->message(message, w, l); }
    catch (const std::exception& e) { showError(window, e); return 0; }
}
INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM w, LPARAM) {
    static std::vector<Stroke> draft;
    if (message == WM_INITDIALOG) {
        const auto p = app->preferences();
        draft = p.custom;
        CheckDlgButton(dialog, IDC_GAME_ONLY, p.scope == Scope::Genshin ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_AUTO_RESTORE, p.timed ? BST_CHECKED : BST_UNCHECKED);
        wchar_t text[64]{}; swprintf_s(text, L"%.9g", p.minutes); SetDlgItemTextW(dialog, IDC_MINUTES, text);
        for (const auto* shortcut : shortcuts) SendDlgItemMessageW(dialog, IDC_SHORTCUT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(shortcut));
        SendDlgItemMessageW(dialog, IDC_SHORTCUT, CB_SETCURSEL, p.shortcut, 0);
        SetDlgItemTextW(dialog, IDC_DETECTION, app->detection().c_str());
        SetDlgItemTextW(dialog, IDC_SEQUENCE, sequenceLabel(draft).c_str());
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(w) == IDC_RECORD) {
        try {
            auto strokes = app->record(dialog);
            if (!strokes.empty()) { draft = std::move(strokes); SendDlgItemMessageW(dialog, IDC_SHORTCUT, CB_SETCURSEL, 3, 0); }
            SetDlgItemTextW(dialog, IDC_SEQUENCE, sequenceLabel(draft).c_str());
        } catch (const std::exception& e) { showError(dialog, e); }
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(w) == IDOK) {
        try {
            Preferences p;
            p.scope = IsDlgButtonChecked(dialog, IDC_GAME_ONLY) == BST_CHECKED ? Scope::Genshin : Scope::System;
            p.timed = IsDlgButtonChecked(dialog, IDC_AUTO_RESTORE) == BST_CHECKED;
            wchar_t text[64]{}; GetDlgItemTextW(dialog, IDC_MINUTES, text, static_cast<int>(std::size(text)));
            p.minutes = parseMinutes(text);
            p.shortcut = static_cast<int>(SendDlgItemMessageW(dialog, IDC_SHORTCUT, CB_GETCURSEL, 0, 0));
            p.custom = draft;
            if (p.shortcut < 0 || p.shortcut > 3) throw std::runtime_error("Choose a shortcut.");
            app->apply(p); EndDialog(dialog, IDOK);
        } catch (const std::exception& e) { showError(dialog, e); }
        return TRUE;
    }
    if ((message == WM_COMMAND && LOWORD(w) == IDCANCEL) || message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
INT_PTR CALLBACK recorderProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_INITDIALOG) { app->recorderWindow(dialog); return TRUE; }
    if (message == RecordedKey) { app->recordKey({static_cast<std::uint16_t>(w), static_cast<std::uint16_t>(l)}, dialog); return TRUE; }
    if (message == WM_COMMAND && LOWORD(w) == IDC_CLEAR) { app->clearRecording(dialog); return TRUE; }
    if (message == WM_COMMAND && LOWORD(w) == IDOK) {
        if (app->hasRecording()) EndDialog(dialog, IDOK);
        return TRUE;
    }
    if ((message == WM_COMMAND && LOWORD(w) == IDCANCEL) || message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (message == WM_DESTROY) app->recorderWindow(nullptr);
    return FALSE;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    Handle singleton{CreateMutexW(nullptr, FALSE, L"Local\\LagSwitchWindows.SingleInstance")};
    if (!singleton.value) { MessageBoxW(nullptr, L"Cannot create the single-instance lock.", L"LagSwitch", MB_OK | MB_ICONERROR); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) { MessageBoxW(nullptr, L"LagSwitch is already running. Look in the notification area, including hidden icons.", L"LagSwitch", MB_OK); return 0; }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    int result{};
    try {
        App running; app = &running; running.start(instance);
        MSG message{};
        BOOL code{};
        while ((code = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (code == -1) result = 1;
        app = nullptr;
    } catch (const std::exception& e) { app = nullptr; showError(nullptr, e); result = 1; }
    WSACleanup();
    return result;
}
