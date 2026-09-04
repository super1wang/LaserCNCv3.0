#pragma once

#include <lasercnc/kernel/identifiers.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace lasercnc::runtime::detail {

// Private seam: no wall clock, document text or process-local counter is an identity input.
// 中文翻译：私有测试接缝；身份不使用墙钟、文档文本或进程局部计数。
template<typename FillRandomWords>
[[nodiscard]] foundation::Result<kernel::SnapshotId> closeSnapshotIdentity(FillRandomWords&& fill)
{
    try {
        std::array<std::uint32_t, 8U> words{};
        fill(words);
        if(!std::all_of(words.begin(), words.end(), [](auto word) { return word == 0U; })) {
            constexpr char hex[] = "0123456789abcdef";
            std::string identity{"snapshot.close.v2."};
            identity.reserve(82U);
            for(const auto word : words) {
                for(unsigned int nibble = 0U; nibble < 8U; ++nibble) {
                    identity.push_back(hex[(word >> (28U - nibble * 4U)) & 0xfU]);
                }
            }
            return kernel::SnapshotId::create(std::move(identity));
        }
    } catch(...) {
        // Never fall back to a clock/PID key if entropy acquisition fails.
        // 中文翻译：熵获取失败时绝不退回时钟或 PID 键。
    }
    return foundation::Result<kernel::SnapshotId>::failure(foundation::makeError(
        "Snapshot.IdentityGenerationFailed", foundation::ErrorCategory::Infrastructure,
        "A nonzero random snapshot identity could not be generated"));
}
} // namespace lasercnc::runtime::detail
