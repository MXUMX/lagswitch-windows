#pragma once
#include <winsock2.h>
#include <windows.h>
#include <initguid.h>
#include <fwpmu.h>
#include <rpc.h>
#include "WfpCompat.h"
#include "Core.h"
#include <sstream>

namespace lag {
inline void winCheck(DWORD code, const char* operation) {
    if (code == ERROR_SUCCESS) return;
    std::ostringstream text;
    text << operation << " failed (0x" << std::hex << code << ").";
    if (code == ERROR_ACCESS_DENIED) text << " Run LagSwitch as administrator.";
    throw std::runtime_error(text.str());
}
class WindowsFilter final : public Backend {
    HANDLE engine_{};
    GUID sublayer_{};
    std::vector<UINT64> ids_;
    void open() {
        if (engine_) return;
        FWPM_SESSION0 session{};
        session.displayData.name = const_cast<wchar_t*>(L"LagSwitch temporary session");
        session.flags = FWPM_SESSION_FLAG_DYNAMIC;
        session.txnWaitTimeoutInMSec = 2000;
        winCheck(FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine_), "Open Windows Filtering Platform");
        try {
            const auto uuid = UuidCreate(&sublayer_);
            if (uuid != RPC_S_OK && uuid != RPC_S_UUID_LOCAL_ONLY) winCheck(uuid, "Create session ID");
            FWPM_SUBLAYER0 sublayer{};
            sublayer.subLayerKey = sublayer_;
            sublayer.displayData.name = const_cast<wchar_t*>(L"LagSwitch temporary packet drops");
            sublayer.weight = 0xFFFF;
            winCheck(FwpmSubLayerAdd0(engine_, &sublayer, nullptr), "Create temporary filter sublayer");
        } catch (...) { close(); throw; }
    }
    UINT64 add(const Rule& rule) {
        FWPM_FILTER_CONDITION0 conditions[3]{};
        UINT32 count{};
        if (!rule.includeLoopback) {
            auto& c = conditions[count++];
            c.fieldKey = FWPM_CONDITION_FLAGS;
            c.matchType = FWP_MATCH_FLAGS_NONE_SET;
            c.conditionValue.type = FWP_UINT32;
            c.conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;
        }
        if (!rule.system) {
            auto& protocol = conditions[count++];
            protocol.fieldKey = FWPM_CONDITION_IP_PROTOCOL;
            protocol.matchType = FWP_MATCH_EQUAL;
            protocol.conditionValue.type = FWP_UINT8;
            protocol.conditionValue.uint8 = IPPROTO_UDP;
            auto& port = conditions[count++];
            port.fieldKey = FWPM_CONDITION_IP_LOCAL_PORT;
            port.matchType = FWP_MATCH_EQUAL;
            port.conditionValue.type = FWP_UINT16;
            port.conditionValue.uint16 = rule.port;
        }
        FWPM_FILTER0 filter{};
        filter.displayData.name = const_cast<wchar_t*>(L"LagSwitch temporary DROP");
        if (rule.system) {
            filter.layerKey = rule.ipv6
                ? (rule.inbound ? FWPM_LAYER_INBOUND_IPPACKET_V6 : FWPM_LAYER_OUTBOUND_IPPACKET_V6)
                : (rule.inbound ? FWPM_LAYER_INBOUND_IPPACKET_V4 : FWPM_LAYER_OUTBOUND_IPPACKET_V4);
        } else {
            filter.layerKey = rule.ipv6
                ? (rule.inbound ? FWPM_LAYER_INBOUND_TRANSPORT_V6 : FWPM_LAYER_OUTBOUND_TRANSPORT_V6)
                : (rule.inbound ? FWPM_LAYER_INBOUND_TRANSPORT_V4 : FWPM_LAYER_OUTBOUND_TRANSPORT_V4);
        }
        filter.subLayerKey = sublayer_;
        filter.action.type = FWP_ACTION_BLOCK;
        UINT64 weight = ~UINT64{0};
        filter.weight.type = FWP_UINT64;
        filter.weight.uint64 = &weight;
        filter.numFilterConditions = count;
        filter.filterCondition = conditions;
        UINT64 id{};
        winCheck(FwpmFilterAdd0(engine_, &filter, nullptr, &id), "Install packet-drop filter");
        return id;
    }
public:
    WindowsFilter() { open(); }
    WindowsFilter(const WindowsFilter&) = delete;
    WindowsFilter& operator=(const WindowsFilter&) = delete;
    ~WindowsFilter() override { close(); }
    void close() noexcept {
        if (engine_) FwpmEngineClose0(engine_);
        engine_ = nullptr;
        ids_.clear();
    }
    void block(const std::vector<Rule>& rules) override {
        if (rules.empty()) throw std::runtime_error("Refusing an empty filter plan.");
        open();
        winCheck(FwpmTransactionBegin0(engine_, 0), "Begin filter transaction");
        std::vector<UINT64> next;
        try {
            for (auto id : ids_) winCheck(FwpmFilterDeleteById0(engine_, id), "Replace own filter");
            for (const auto& r : rules) next.push_back(add(r));
            winCheck(FwpmTransactionCommit0(engine_), "Commit packet drops");
            ids_ = std::move(next);
        } catch (...) { FwpmTransactionAbort0(engine_); throw; }
    }
    void restore() override {
        if (!engine_ || ids_.empty()) return;
        try {
            winCheck(FwpmTransactionBegin0(engine_, 0), "Begin restore transaction");
            for (auto id : ids_) winCheck(FwpmFilterDeleteById0(engine_, id), "Remove own filter");
            winCheck(FwpmTransactionCommit0(engine_), "Commit traffic restoration");
            ids_.clear();
        } catch (...) {
            FwpmTransactionAbort0(engine_);
            // Closing a dynamic session deletes its objects, even if individual
            // filter deletion fails. A subsequent activation opens a new session.
            close();
            throw;
        }
    }
};
} // namespace lag
