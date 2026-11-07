#pragma once
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lag {
// Only the explicitly recorded shortcut is persisted; no keyboard history.
struct Stroke {
    std::uint16_t key{}, modifiers{}; // Ctrl=1, Alt=2, Shift=4, Win=8.
    bool operator==(const Stroke& b) const { return key == b.key && modifiers == b.modifiers; }
};
inline bool validSequence(const std::vector<Stroke>& strokes) {
    if (strokes.empty() || strokes.size() > 4) return false;
    for (const auto& s : strokes) if (!s.key || s.key > 255 || s.modifiers > 15) return false;
    return true;
}
inline std::wstring encodeSequence(const std::vector<Stroke>& strokes) {
    std::wstring result;
    for (const auto& s : strokes) result += std::to_wstring(s.key) + L"/" + std::to_wstring(s.modifiers) + L";";
    return result;
}
inline std::vector<Stroke> decodeSequence(const std::wstring& text) {
    std::vector<Stroke> result;
    std::wistringstream input(text);
    unsigned key{}, modifiers{}; wchar_t slash{}, end{};
    while (input.peek() != std::char_traits<wchar_t>::eof()) {
        if (!(input >> key >> slash >> modifiers >> end) || slash != L'/' || end != L';' || key > 255 || modifiers > 15)
            throw std::runtime_error("Invalid saved shortcut sequence.");
        result.push_back({static_cast<std::uint16_t>(key), static_cast<std::uint16_t>(modifiers)});
    }
    if (!validSequence(result)) throw std::runtime_error("Record between one and four shortcut strokes.");
    return result;
}
class SequenceMatcher {
    std::vector<Stroke> sequence_;
    std::vector<std::size_t> failure_;
    std::size_t index_{};
    std::uint64_t last_{};
public:
    void set(const std::vector<Stroke>& strokes) {
        sequence_ = strokes; failure_.assign(strokes.size(), 0); reset();
        for (std::size_t i = 1; i < strokes.size(); ++i) {
            auto prefix = failure_[i - 1];
            while (prefix && !(strokes[i] == strokes[prefix])) prefix = failure_[prefix - 1];
            if (strokes[i] == strokes[prefix]) ++prefix;
            failure_[i] = prefix;
        }
    }
    void reset() { index_ = 0; last_ = 0; }
    bool key(const Stroke& stroke, std::uint64_t now) {
        if (sequence_.empty()) return false;
        if (index_ && (now < last_ || now - last_ > 1000)) index_ = 0;
        while (index_ && !(stroke == sequence_[index_])) index_ = failure_[index_ - 1];
        if (stroke == sequence_[index_]) {
            ++index_; last_ = now;
            if (index_ == sequence_.size()) { reset(); return true; }
        }
        return false;
    }
};
} // namespace lag
