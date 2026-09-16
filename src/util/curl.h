#pragma once

#include <curl/curl.h>
#include <memory>
#include <string>

namespace cha {

// Own request resources; HTTP options and transfer behavior stay with the caller.
class CurlHandle {
public:
    CurlHandle();
    CURL* get() const noexcept { return handle_.get(); }

private:
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle_;
};

class CurlHeaders {
public:
    CurlHeaders() = default;
    ~CurlHeaders() { curl_slist_free_all(headers_); }
    CurlHeaders(const CurlHeaders&) = delete;
    CurlHeaders& operator=(const CurlHeaders&) = delete;

    void append(const std::string& header);
    curl_slist* get() const noexcept { return headers_; }

private:
    curl_slist* headers_{};
};

} // namespace cha
