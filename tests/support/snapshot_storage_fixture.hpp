#pragma once
#include <lasercnc/infrastructure/sha256_hash_service.hpp>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace lasercnc::test {
inline std::filesystem::path snapshotStoragePath(const std::filesystem::path& root, std::string_view id)
{
    auto digest = infrastructure::Sha256HashService{}.digest(std::as_bytes(std::span{id.data(), id.size()}));
    if(!digest) { throw std::runtime_error(digest.error().message); }
    return root / ("@" + std::string(digest.value().value().substr(7U)) + ".snapshot");
}
inline std::string snapshotStorageEnvelope(std::string_view id, std::string_view payload)
{
    std::string result{"LCNCSN02"};
    const auto length = static_cast<std::uint32_t>(id.size());
    for(const auto shift : {24U, 16U, 8U, 0U}) { result.push_back(static_cast<char>((length >> shift) & 0xffU)); }
    result.append(id);
    result.append(payload);
    return result;
}
} // namespace lasercnc::test
