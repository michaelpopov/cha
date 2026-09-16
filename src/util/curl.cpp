#include "util/curl.h"

#include <stdexcept>

namespace cha {
namespace {
class CurlGlobal {
public:
    CurlGlobal() {
        const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error("Failed to initialize libcurl: "
                + std::string(curl_easy_strerror(result)));
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};
} // namespace

CurlHandle::CurlHandle() : handle_(nullptr, curl_easy_cleanup) {
    static const CurlGlobal global;
    handle_.reset(curl_easy_init());
    if (!handle_) throw std::runtime_error("Failed to create libcurl handle");
}

void CurlHeaders::append(const std::string& header) {
    curl_slist* const appended = curl_slist_append(headers_, header.c_str());
    if (!appended) throw std::runtime_error("Failed to create HTTP headers");
    headers_ = appended;
}

} // namespace cha
