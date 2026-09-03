#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/platform/hash_service.hpp>
#include <lasercnc/platform/snapshot_store.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace lasercnc::test {

class FaultHashService final : public platform::IHashService {
public:
    explicit FaultHashService(std::shared_ptr<platform::IHashService> delegate) : delegate_(std::move(delegate)) {}
    void arm(std::string fragment, unsigned int occurrence, bool throws, std::string excluded = {})
    { fragment_ = std::move(fragment); excluded_ = std::move(excluded); remaining_ = occurrence; throws_ = throws; hits = 0U; }
    foundation::Result<kernel::ContentDigest> digest(std::span<const std::byte> content) const override
    {
        const std::string_view text{reinterpret_cast<const char*>(content.data()), content.size()};
        if(remaining_ != 0U && text.find(fragment_) != std::string_view::npos
           && (excluded_.empty() || text.find(excluded_) == std::string_view::npos) && --remaining_ == 0U) {
            ++hits;
            if(throws_) { throw std::runtime_error("Injected hash exception"); }
            return foundation::Result<kernel::ContentDigest>::failure(foundation::makeError(
                "Test.HashStageFailed", foundation::ErrorCategory::Infrastructure, "Injected hash failure"));
        }
        return delegate_->digest(content);
    }
    mutable unsigned int hits{0U};
private:
    std::shared_ptr<platform::IHashService> delegate_;
    std::string fragment_;
    std::string excluded_;
    mutable unsigned int remaining_{0U};
    bool throws_{false};
};

enum class SnapshotFault { None, BeforeWrite, AfterWrite, Read };

class FaultSnapshotStore final : public platform::ISnapshotStore {
public:
    explicit FaultSnapshotStore(std::unique_ptr<platform::ISnapshotStore> delegate) : delegate_(std::move(delegate)) {}
    void arm(SnapshotFault point, bool throws) { point_ = point; throws_ = throws; hits = 0U; }
    foundation::Result<platform::SnapshotWriteDisposition> writeAtomically(
        const kernel::SnapshotId& id, std::string_view payload) override
    {
        if(point_ == SnapshotFault::BeforeWrite) { return failure<platform::SnapshotWriteDisposition>(); }
        auto written = delegate_->writeAtomically(id, payload);
        if(written && point_ == SnapshotFault::AfterWrite) { return failure<platform::SnapshotWriteDisposition>(); }
        return written;
    }
    foundation::Result<std::string> read(const kernel::SnapshotId& id) const override
    {
        if(point_ == SnapshotFault::Read) { return failure<std::string>(); }
        return delegate_->read(id);
    }
    foundation::Result<bool> remove(const kernel::SnapshotId& id) override { return delegate_->remove(id); }
    mutable unsigned int hits{0U};
private:
    template<typename T> foundation::Result<T> failure() const
    {
        point_ = SnapshotFault::None;
        ++hits;
        if(throws_) { throw std::runtime_error("Injected snapshot store exception"); }
        return foundation::Result<T>::failure(foundation::makeError(
            "Test.SnapshotStageFailed", foundation::ErrorCategory::Infrastructure, "Injected snapshot store failure"));
    }
    std::unique_ptr<platform::ISnapshotStore> delegate_;
    mutable SnapshotFault point_{SnapshotFault::None};
    bool throws_{false};
};

} // namespace lasercnc::test
