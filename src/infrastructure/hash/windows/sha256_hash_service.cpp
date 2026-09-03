#include <lasercnc/infrastructure/sha256_hash_service.hpp>

#include <lasercnc/foundation/error.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace lasercnc::infrastructure {
namespace {

foundation::Error hashError(const char* operation, NTSTATUS status)
{
    return foundation::makeError(
        "Hash.ProviderFailed",
        foundation::ErrorCategory::Infrastructure,
        "The system hash provider could not compute a content digest",
        foundation::Value {foundation::Value::Object {
            {"operation", foundation::Value {operation}},
            {"provider", foundation::Value {"windows-cng"}},
            {"status", foundation::Value {static_cast<std::int64_t>(status)}},
        }});
}

struct AlgorithmCloser final {
    void operator()(void* handle) const noexcept
    {
        if(handle != nullptr) {
            static_cast<void>(BCryptCloseAlgorithmProvider(handle, 0U));
        }
    }
};

struct HashCloser final {
    void operator()(void* handle) const noexcept
    {
        if(handle != nullptr) {
            static_cast<void>(BCryptDestroyHash(handle));
        }
    }
};

using AlgorithmHandle = std::unique_ptr<void, AlgorithmCloser>;
using HashHandle = std::unique_ptr<void, HashCloser>;

bool succeeded(NTSTATUS status) noexcept
{
    return status >= 0;
}

std::string lowerHex(const std::vector<unsigned char>& bytes)
{
    constexpr std::array digits {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.resize(bytes.size() * 2U);
    for(std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    return result;
}

} // namespace

foundation::Result<kernel::ContentDigest> Sha256HashService::digest(
    std::span<const std::byte> content) const
{
    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    auto status = BCryptOpenAlgorithmProvider(
        &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
    if(!succeeded(status)) {
        return foundation::Result<kernel::ContentDigest>::failure(
            hashError("open-algorithm", status));
    }
    AlgorithmHandle algorithm(rawAlgorithm);

    DWORD hashLength = 0U;
    DWORD returnedBytes = 0U;
    status = BCryptGetProperty(
        algorithm.get(),
        BCRYPT_HASH_LENGTH,
        reinterpret_cast<unsigned char*>(&hashLength),
        sizeof(hashLength),
        &returnedBytes,
        0U);
    if(!succeeded(status) || returnedBytes != sizeof(hashLength) || hashLength == 0U) {
        return foundation::Result<kernel::ContentDigest>::failure(
            hashError("read-hash-length", status));
    }

    BCRYPT_HASH_HANDLE rawHash = nullptr;
    status = BCryptCreateHash(
        algorithm.get(), &rawHash, nullptr, 0U, nullptr, 0U, 0U);
    if(!succeeded(status)) {
        return foundation::Result<kernel::ContentDigest>::failure(
            hashError("create-hash", status));
    }
    HashHandle hash(rawHash);

    auto remaining = content;
    while(!remaining.empty()) {
        const auto chunkSize = std::min<std::size_t>(
            remaining.size(), static_cast<std::size_t>(std::numeric_limits<ULONG>::max()));
        status = BCryptHashData(
            hash.get(),
            reinterpret_cast<unsigned char*>(const_cast<std::byte*>(remaining.data())),
            static_cast<ULONG>(chunkSize),
            0U);
        if(!succeeded(status)) {
            return foundation::Result<kernel::ContentDigest>::failure(
                hashError("hash-data", status));
        }
        remaining = remaining.subspan(chunkSize);
    }

    std::vector<unsigned char> digest(hashLength);
    status = BCryptFinishHash(hash.get(), digest.data(), hashLength, 0U);
    if(!succeeded(status)) {
        return foundation::Result<kernel::ContentDigest>::failure(
            hashError("finish-hash", status));
    }
    return kernel::ContentDigest::create("sha256:" + lowerHex(digest));
}

} // namespace lasercnc::infrastructure
