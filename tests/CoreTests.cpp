#include "Core.h"
#include "Shortcut.h"
#include "ToggleMailbox.h"
#include <iostream>
#include <limits>
#include <thread>
#include <atomic>
using namespace lag;
int checks{};
void check(bool value, const char* name) { ++checks; if (!value) throw std::runtime_error(name); }
template<class F> void rejects(F f, const char* name) { bool rejected{}; try { f(); } catch (...) { rejected = true; } check(rejected, name); }
struct Mock : Backend {
    bool active{}, failBlock{};
    std::vector<Rule> rules;
    int blocks{}, restores{};
    void block(const std::vector<Rule>& value) override {
        if (failBlock) throw std::runtime_error("Injected transaction failure");
        rules = value; active = true; ++blocks;
    }
    void restore() override { active = false; ++restores; }
};
int main() {
    try {
        const Stroke a{65, 1}, b{66, 1}, c{67, 1};
        SequenceMatcher matcher;
        matcher.set({a, b});
        check(!matcher.key(a, 1000) && matcher.key(b, 1100), "ordered sequence fires on final stroke");
        check(!matcher.key(a, 2000) && !matcher.key(b, 3001), "sequence gap timeout");
        check(!matcher.key(a, 4000) && !matcher.key(c, 4100) && !matcher.key(b, 4200), "unrelated keys reset sequence");
        matcher.set({a, a, b});
        check(!matcher.key(a, 5000) && !matcher.key(a, 5050) && !matcher.key(a, 5100) && matcher.key(b, 5150), "repeated-key overlapping prefix");
        matcher.set({a});
        check(matcher.key(a, 6000) && matcher.key(a, 6100), "single chord toggles on successive presses");
        const auto encoded = encodeSequence({a, b});
        check(decodeSequence(encoded) == std::vector<Stroke>({a, b}), "custom shortcut persistence");
        for (const auto* text : {L"", L"0/1;", L"256/1;", L"65/16;", L"65/1;garbage", L"65/1;65/1;65/1;65/1;65/1;"}) rejects([&] { decodeSequence(text); }, "invalid saved sequence");
        ToggleMailbox mailbox;
        mailbox.toggle();
        auto first = mailbox.take();
        check(first.request == ToggleMailbox::Request::Toggle, "worker receives immediate toggle");
        // Simulate 1000 further taps while the first operation is in flight.
        for (int i = 0; i < 1000; ++i) mailbox.toggle();
        mailbox.toggle();
        check(mailbox.take().request == ToggleMailbox::Request::Toggle, "rapid even taps cancel; one final odd tap remains");
        mailbox.toggle(); mailbox.restore(); mailbox.toggle();
        check(mailbox.take().request == ToggleMailbox::Request::Restore, "restore wins over waiting taps");
        mailbox.toggle(); auto obsolete = mailbox.take(); mailbox.clear();
        check(!mailbox.current(obsolete), "settings changes discard already-dequeued stale work");
        mailbox.stop();
        check(mailbox.take().request == ToggleMailbox::Request::Stop, "shutdown discards pending work");
        check(parseMinutes(L"0.5") == 0.5, "fractional minutes");
        check(parseMinutes(L" 2.5 \t") == 2.5, "whitespace");
        for (const auto* text : {L"", L"0", L"-1", L"nan", L"inf", L"1441", L"0.5junk"}) rejects([&] { parseMinutes(text); }, "invalid duration");
        check(validMinutes(1440), "duration upper bound");
        auto rules = plan(Scope::Genshin, {{false, 50000}, {true, 50000}});
        check(rules.size() == 4, "both IP families and directions");
        for (const auto& r : rules) check(!r.system && r.port == 50000 && !r.includeLoopback, "game scope remains UDP port and excludes loopback");
        rejects([] { plan(Scope::Genshin, {}); }, "no fallback to system");
        rejects([] { plan(Scope::Genshin, {{false, 0}}); }, "no zero port");
        rules = plan(Scope::System, {});
        check(rules.size() == 4, "system four layers");
        for (const auto& r : rules) check(r.system && !r.port && !r.includeLoopback, "system rules exclude loopback");
        Mock backend;
        Controller control(backend);
        control.heartbeat(1000);
        check(!control.status(1000).ready, "starts scanning");
        rejects([&] { control.toggle(1000); }, "cold activation refused");
        control.updateDiscovery({{false, 45678}}, 1000);
        check(control.status(1001).ready, "fresh target ready");
        control.toggle(1001);
        check(backend.active && control.status(1001).blocked, "activation");
        control.updateDiscovery({}, 1002, "game closed");
        check(control.status(1002).blocked && backend.rules[0].port == 45678, "active rules frozen during discovery changes");
        control.toggle(1003);
        check(!backend.active, "manual restore");
        rejects([&] { control.toggle(1003); }, "failed scan invalidates cache");
        control.updateDiscovery({{false, 45678}}, 1000);
        rejects([&] { control.toggle(6001); }, "stale scan refused");
        control.configure(Scope::System, true, 0.5, 2000);
        control.toggle(2000);
        control.tick(1999);
        check(backend.active && control.status(1999).remaining == 30000, "stale watchdog timestamp cannot expire a newer activation");
        control.heartbeat(31999); control.tick(31999);
        check(backend.active, "timer does not expire early");
        control.tick(32000);
        check(!backend.active, "fractional timer restores at deadline");
        control.toggle(33000);
        control.configure(Scope::System, true, 0.01, 34000);
        check(!backend.active, "shortening expired timer restores immediately");
        control.configure(Scope::System, false, 0.5, 35000);
        control.toggle(35000); control.heartbeat(100000); control.tick(100000);
        check(backend.active, "disabled timer keeps block");
        control.configure(Scope::Genshin, false, 0.5, 100001);
        check(!backend.active, "scope change restores first");
        backend.failBlock = true;
        control.updateDiscovery({{true, 34567}}, 100001);
        rejects([&] { control.toggle(100001); }, "backend failure reported");
        check(!control.status(100001).blocked, "failed transaction not reported blocked");
        backend.failBlock = false;
        control.toggle(100001); control.heartbeat(100001); control.tick(106002);
        check(!backend.active, "watchdog restores without UI heartbeat");
        rejects([&] { control.toggle(106002); }, "queued toggle cannot reblock after watchdog");
        control.heartbeat(107000); control.updateDiscovery({{false, 12345}}, 107000);
        control.toggle(107000);
        check(backend.active, "recovers after watchdog");
        control.restore(); control.restore();
        check(!backend.active, "restore is idempotent");
        std::atomic<bool> running{true};
        std::thread scanner([&] { for (int i = 0; i < 1000; ++i) control.updateDiscovery({{false, 12345}}, 108000); running = false; });
        while (running) { control.status(108001); control.heartbeat(108001); }
        scanner.join();
        check(control.status(108001).ready, "concurrent cache updates");
        for (int i = 0; i < 1001; ++i) control.toggle(108001);
        check(control.status(108001).blocked && backend.active, "1001 rapid toggles preserve odd parity");
        control.toggle(108001);
        check(!control.status(108001).blocked && !backend.active, "1002 rapid toggles restore with no leftover block");
        std::cout << "PASS: " << checks << " core checks; no live firewall changes.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
