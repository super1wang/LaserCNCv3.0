#pragma once

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/platform/persistence_backend.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace lasercnc::test {

enum class BackendPoint { Begin, Execute, Query, Commit, Rollback, HostSession };

// Faults happen before delegation; a failed commit has not committed in SQLite.
// 中文翻译：在委托调用前注入故障，提交失败表示 SQLite 尚未提交。
class FaultInjectingBackend final : public platform::IPersistenceBackend {
public:
    explicit FaultInjectingBackend(std::unique_ptr<platform::IPersistenceBackend> delegate)
        : delegate_(std::move(delegate)) {}

    foundation::Result<platform::PersistenceSessionInfo> acquireHostSession() override
    {
        auto checked = check(BackendPoint::HostSession);
        if(!checked) { return foundation::Result<platform::PersistenceSessionInfo>::failure(std::move(checked).error()); }
        return delegate_->acquireHostSession();
    }

    void arm(BackendPoint point, std::string fragment, unsigned int occurrence, bool throws)
    {
        point_ = point;
        fragment_ = std::move(fragment);
        remaining_ = occurrence;
        throws_ = throws;
        hits = 0U;
    }

    foundation::Result<std::size_t> execute(std::string_view sql,
        std::span<const foundation::Value> parameters = {}) override
    {
        auto checked = check(BackendPoint::Execute, sql);
        if(!checked) { return foundation::Result<std::size_t>::failure(std::move(checked).error()); }
        transactionSql_.append(sql);
        transactionSql_.push_back('\n');
        return delegate_->execute(sql, parameters);
    }
    foundation::Result<std::vector<platform::PersistenceRow>> query(std::string_view sql,
        std::span<const foundation::Value> parameters = {}) override
    {
        auto checked = check(BackendPoint::Query, sql);
        if(!checked) { return foundation::Result<std::vector<platform::PersistenceRow>>::failure(std::move(checked).error()); }
        return delegate_->query(sql, parameters);
    }
    foundation::Result<void> beginTransaction() override
    {
        auto checked = check(BackendPoint::Begin);
        transactionSql_.clear();
        return checked ? delegate_->beginTransaction() : checked;
    }
    foundation::Result<void> commitTransaction() override
    {
        auto checked = check(BackendPoint::Commit, transactionSql_);
        return checked ? delegate_->commitTransaction() : checked;
    }
    foundation::Result<void> rollbackTransaction() override
    {
        if(failRollback) {
            failRollback = false;
            ++rollbackHits;
            if(throwRollback) { throw std::runtime_error("Injected rollback exception"); }
            return foundation::Result<void>::failure(foundation::makeError(
                "Test.RollbackFailure", foundation::ErrorCategory::Infrastructure,
                "Injected rollback failure before SQLite rollback"));
        }
        auto checked = check(BackendPoint::Rollback);
        return checked ? delegate_->rollbackTransaction() : checked;
    }
    unsigned int hits{0U};
    bool failRollback{false};
    bool throwRollback{false};
    unsigned int rollbackHits{0U};

private:
    foundation::Result<void> check(BackendPoint point, std::string_view sql = {})
    {
        if(remaining_ != 0U && point == point_ && sql.find(fragment_) != std::string_view::npos
           && --remaining_ == 0U) {
            ++hits;
            if(throws_) { throw std::runtime_error("Injected backend stage exception"); }
            return foundation::Result<void>::failure(foundation::makeError(
                "Test.BackendStageFailure", foundation::ErrorCategory::Infrastructure,
                "Injected backend stage failure"));
        }
        return foundation::Result<void>::success();
    }
    std::unique_ptr<platform::IPersistenceBackend> delegate_;
    BackendPoint point_{BackendPoint::Begin};
    std::string fragment_;
    std::string transactionSql_;
    unsigned int remaining_{0U};
    bool throws_{false};
};

} // namespace lasercnc::test
