#pragma once
#include <condition_variable>
#include <mutex>
#include <cstdint>

namespace lag {
// At most one operation is running and one intent is waiting. Two additional
// presses cancel by parity; an explicit restoration overrides waiting toggles.
class ToggleMailbox {
public:
    enum class Request { Stop, Toggle, Restore };
    struct Ticket { Request request; std::uint64_t generation; };
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool toggle_{}, restore_{}, stopped_{};
    std::uint64_t generation_{};
public:
    void toggle() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopped_ && !restore_) toggle_ = !toggle_;
        changed_.notify_one();
    }
    void restore() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopped_) { restore_ = true; toggle_ = false; }
        changed_.notify_one();
    }
    void clear() { std::lock_guard<std::mutex> lock(mutex_); toggle_ = restore_ = false; ++generation_; }
    bool current(const Ticket& ticket) { std::lock_guard<std::mutex> lock(mutex_); return !stopped_ && ticket.generation == generation_; }
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true; toggle_ = restore_ = false; changed_.notify_all();
    }
    Ticket take() {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return stopped_ || toggle_ || restore_; });
        if (stopped_) return {Request::Stop, generation_};
        if (restore_) { restore_ = false; return {Request::Restore, generation_}; }
        toggle_ = false; return {Request::Toggle, generation_};
    }
};
} // namespace lag
