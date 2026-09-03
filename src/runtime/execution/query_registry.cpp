#include <lasercnc/runtime/query_registry.hpp>

#include <lasercnc/foundation/error.hpp>

#include <mutex>
#include <string>
#include <utility>

namespace lasercnc::runtime {
namespace {

foundation::Error queryError(
    const char* code,
    foundation::ErrorCategory category,
    const char* message,
    const kernel::QueryName& name)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"query", foundation::Value {std::string(name.value())}},
        }});
}

} // namespace

foundation::Result<void> QueryRegistry::registerHandler(
    QueryDescriptor descriptor,
    std::shared_ptr<IQueryHandler> handler)
{
    if(handler == nullptr) {
        return foundation::Result<void>::failure(queryError(
            "Query.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "A query handler is required",
            descriptor.name));
    }
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(queryError(
            "Query.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Query registration is closed",
            descriptor.name));
    }
    const auto name = descriptor.name;
    const auto [unused, inserted] = entries_.emplace(
        name, Entry {std::move(descriptor), std::move(handler)});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(queryError(
            "Query.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "A query with the same stable name is already registered",
            name));
    }
    return foundation::Result<void>::success();
}

foundation::Result<QueryDescriptor> QueryRegistry::descriptor(
    const kernel::QueryName& name) const
{
    auto entry = resolve(name);
    if(!entry.hasValue()) {
        return foundation::Result<QueryDescriptor>::failure(std::move(entry).error());
    }
    return foundation::Result<QueryDescriptor>::success(std::move(entry).value().descriptor);
}

std::vector<QueryDescriptor> QueryRegistry::descriptors() const
{
    std::shared_lock lock(mutex_);
    std::vector<QueryDescriptor> result;
    result.reserve(entries_.size());
    for(const auto& [unused, entry] : entries_) {
        static_cast<void>(unused);
        result.push_back(entry.descriptor);
    }
    return result;
}

std::size_t QueryRegistry::size() const
{
    std::shared_lock lock(mutex_);
    return entries_.size();
}

bool QueryRegistry::frozen() const
{
    std::shared_lock lock(mutex_);
    return frozen_;
}

foundation::Result<QueryRegistry::Entry> QueryRegistry::resolve(
    const kernel::QueryName& name) const
{
    std::shared_lock lock(mutex_);
    const auto entry = entries_.find(name);
    if(entry == entries_.end()) {
        return foundation::Result<Entry>::failure(queryError(
            "Query.NotFound",
            foundation::ErrorCategory::NotFound,
            "The query is not registered",
            name));
    }
    return foundation::Result<Entry>::success(entry->second);
}

void QueryRegistry::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

} // namespace lasercnc::runtime
