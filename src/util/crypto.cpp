#include "util/crypto.h"

#include "util/path_name.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <memory>
#endif

namespace cha {
namespace {

#ifdef _WIN32

class AlgorithmHandle {
public:
    explicit AlgorithmHandle(ULONG flags = 0) {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &handle_, BCRYPT_SHA256_ALGORITHM, nullptr, flags))) {
            throw std::runtime_error("Failed to open SHA-256 provider");
        }
    }

    ~AlgorithmHandle() {
        if (handle_) BCryptCloseAlgorithmProvider(handle_, 0);
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_{};
};

class HashHandle {
public:
    HashHandle(const AlgorithmHandle& algorithm, std::string_view key = {}) {
        if (key.size() > std::numeric_limits<ULONG>::max()) {
            throw std::runtime_error("SHA-256 key is too large");
        }
        auto* key_data = reinterpret_cast<PUCHAR>(
            const_cast<char*>(key.data()));
        if (!BCRYPT_SUCCESS(BCryptCreateHash(
                algorithm.get(), &handle_, nullptr, 0, key_data,
                static_cast<ULONG>(key.size()), 0))) {
            throw std::runtime_error("Failed to initialize SHA-256 hash");
        }
    }

    ~HashHandle() {
        if (handle_) BCryptDestroyHash(handle_);
    }

    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;

    void update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (size > 0) {
            const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(
                size, std::numeric_limits<ULONG>::max()));
            if (!BCRYPT_SUCCESS(BCryptHashData(
                    handle_, const_cast<PUCHAR>(bytes), chunk, 0))) {
                throw std::runtime_error("Failed to update SHA-256 hash");
            }
            bytes += chunk;
            size -= chunk;
        }
    }

    Sha256Digest finish() {
        Sha256Digest digest{};
        if (!BCRYPT_SUCCESS(BCryptFinishHash(
                handle_, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            throw std::runtime_error("Failed to finish SHA-256 hash");
        }
        return digest;
    }

private:
    BCRYPT_HASH_HANDLE handle_{};
};

#else

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

DigestContext digest_context() {
    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("Failed to initialize SHA-256 hash");
    }
    return context;
}

Sha256Digest finish_digest(EVP_MD_CTX* context) {
    Sha256Digest digest{};
    unsigned int size{};
    if (EVP_DigestFinal_ex(context, digest.data(), &size) != 1
        || size != digest.size()) {
        throw std::runtime_error("Failed to finish SHA-256 hash");
    }
    return digest;
}

#endif

} // namespace

Sha256Digest sha256_digest(std::string_view contents) {
#ifdef _WIN32
    const AlgorithmHandle algorithm;
    HashHandle hash(algorithm);
    hash.update(contents.data(), contents.size());
    return hash.finish();
#else
    DigestContext context = digest_context();
    if (EVP_DigestUpdate(context.get(), contents.data(), contents.size()) != 1) {
        throw std::runtime_error("Failed to update SHA-256 hash");
    }
    return finish_digest(context.get());
#endif
}

Sha256Digest sha256_file_digest(const std::filesystem::path& path) {
#ifdef _WIN32
    const AlgorithmHandle algorithm;
    HashHandle hash(algorithm);
#else
    DigestContext context = digest_context();
#endif

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read file '" + utf8_path(path) + "' for SHA-256");
    }
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes = input.gcount();
        if (bytes > 0) {
#ifdef _WIN32
            hash.update(buffer.data(), static_cast<std::size_t>(bytes));
#else
            if (EVP_DigestUpdate(
                    context.get(), buffer.data(), static_cast<std::size_t>(bytes))
                != 1) {
                throw std::runtime_error("Failed to update SHA-256 hash");
            }
#endif
        }
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "Failed while reading file '" + utf8_path(path) + "'");
    }

#ifdef _WIN32
    return hash.finish();
#else
    return finish_digest(context.get());
#endif
}

Sha256Digest hmac_sha256_digest(
    std::string_view key,
    std::string_view contents) {
#ifdef _WIN32
    const AlgorithmHandle algorithm(BCRYPT_ALG_HANDLE_HMAC_FLAG);
    HashHandle hash(algorithm, key);
    hash.update(contents.data(), contents.size());
    return hash.finish();
#else
    if (key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("SHA-256 key is too large");
    }
    Sha256Digest digest{};
    unsigned int size{};
    if (HMAC(
            EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(contents.data()),
            contents.size(), digest.data(), &size) == nullptr
        || size != digest.size()) {
        throw std::runtime_error("Failed to calculate SHA-256 HMAC");
    }
    return digest;
#endif
}

} // namespace cha
