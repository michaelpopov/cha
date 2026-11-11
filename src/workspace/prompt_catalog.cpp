#include "workspace/prompt_catalog.h"

#include "util/text_file.h"

#include <algorithm>

namespace cha {

std::string_view embedded_compression_prompt();
std::string_view embedded_summary_l1_prompt();
std::string_view embedded_summary_l2_prompt();
std::string_view embedded_summary_l3_section_prompt();
std::string_view embedded_summary_l3_overview_prompt();
std::string_view embedded_session_short_summary_prompt();
std::string_view embedded_session_long_summary_prompt();
std::string_view embedded_page_formatting_prompt();
std::string_view embedded_youtube_transcript_prompt();

const std::array<PromptCatalogEntry, 9> prompt_catalog{{
    {"note-compression", "Note compression", embedded_compression_prompt},
    {"summary-l1", "Summary L1", embedded_summary_l1_prompt},
    {"summary-l2", "Summary L2", embedded_summary_l2_prompt},
    {"summary-l3-section", "Summary L3 section", embedded_summary_l3_section_prompt},
    {"summary-l3-overview", "Summary L3 overview", embedded_summary_l3_overview_prompt},
    {"session-short-summary", "Short session summary", embedded_session_short_summary_prompt},
    {"session-long-summary", "Long session summary", embedded_session_long_summary_prompt},
    {"page-formatting", "Page formatting", embedded_page_formatting_prompt},
    {"youtube-transcript", "YouTube transcript formatting", embedded_youtube_transcript_prompt},
}};

std::string PromptCatalogEntry::path() const {
    return std::string(prompt_config_prefix) + std::string(id) + ".md";
}

const PromptCatalogEntry* find_prompt_catalog_entry(std::string_view id) {
    const auto found = std::ranges::find(prompt_catalog, id, &PromptCatalogEntry::id);
    return found == prompt_catalog.end() ? nullptr : &*found;
}

std::string normalize_prompt_text(std::string_view text) {
    validate_text_file(text, "Prompt text is too large. The limit is 256 KiB.", true);
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            normalized.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            normalized.push_back(text[i]);
        }
    }
    return normalized;
}

std::vector<ConfigFile> default_prompt_config_files() {
    std::vector<ConfigFile> rows;
    for (const auto& entry : prompt_catalog) {
        rows.push_back({entry.path(), normalize_prompt_text(entry.embedded_text())});
    }
    return rows;
}

} // namespace cha
