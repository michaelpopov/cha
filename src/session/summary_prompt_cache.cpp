#include "session/summary_prompt_cache.h"

#include "util/logging.h"

#include <utility>

namespace cha {

SummaryPromptCache::SummaryPromptCache(Reader reader, Clock clock)
    : reader_(std::move(reader)), clock_(std::move(clock)) {}

std::string SummaryPromptCache::load(const std::atomic_bool& cancellation) {
    if (cancellation.load()) return {};
    std::lock_guard lock(mutex_);
    if (cancellation.load()) return {};
    const auto now = clock_();
    if (entry_ && now - entry_->created_at < std::chrono::minutes{10})
        return entry_->section;
    auto section = reader_(cancellation);
    if (section) {
        entry_ = Entry{std::move(*section), clock_()};
        return entry_->section;
    }
    if (!cancellation.load()) log_warn("Saved summary catalogue refresh failed");
    return {};
}

} // namespace cha
