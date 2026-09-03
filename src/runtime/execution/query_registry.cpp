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
    const QueryKey& key)
{
    return foundation::makeError(
        code,
        category,
        message,
        foundation::Value {foundation::Value::Object {
            {"query", foundation::Value {std::string(key.name.value())}},
            {"version", foundation::Value {key.version.toString()}},
        }});
}

QueryKey keyOf(const QueryDescriptor& descriptor)
{
    return QueryKey {descriptor.name, descriptor.version};
}

} // namespace

foundation::Result<void> QueryRegistry::registerHandler(
    QueryDescriptor descriptor,
    std::shared_ptr<IQueryHandler> handler)
{
    const auto key = keyOf(descriptor);
    if(handler == nullptr) {
        return foundation::Result<void>::failure(queryError(
            "Query.InvalidHandler",
            foundation::ErrorCategory::Validation,
            "A query handler is required",
            key));
    }
    std::unique_lock lock(mutex_);
    if(frozen_) {
        return foundation::Result<void>::failure(queryError(
            "Query.RegistryFrozen",
            foundation::ErrorCategory::Conflict,
            "Query registration is closed",
            key));
    }
    const auto [unused, inserted] = entries_.emplace(
        key, Entry {std::move(descriptor), std::move(handler)});
    static_cast<void>(unused);
    if(!inserted) {
        return foundation::Result<void>::failure(queryError(
            "Query.AlreadyRegistered",
            foundation::ErrorCategory::Conflict,
            "The exact query name and version are already registered",
            key));
    }
    return foundation::Result<void>::success();
}

foundation::Result<QueryDescriptor> QueryRegistry::descriptor(
    const QueryKey& key,
    VersionResolution resolution) const
{
    auto entry = resolve(key, resolution);
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
    const QueryKey& key,
    VersionResolution resolution) const
{
    std::shared_lock lock(mutex_);
    const auto exact = entries_.find(key);
    if(resolution == VersionResolution::Exact && exact != entries_.end()) {
        return foundation::Result<Entry>::success(exact->second);
    }

    const auto first = entries_.lower_bound(QueryKey {key.name, foundation::Version {}});
    if(first == entries_.end() || first->first.name != key.name) {
        return foundation::Result<Entry>::failure(queryError(
            "Query.NotFound",
            foundation::ErrorCategory::NotFound,
            "The query is not registered",
            key));
    }
    if(resolution == VersionResolution::Compatible) {
        const Entry* compatible = nullptr;
        for(auto current = first;
            current != entries_.end() && current->first.name == key.name;
            ++current) {
            if(current->first.version.major == key.version.major
               && current->first.version >= key.version) {
                compatible = &current->second;
            }
        }
        if(compatible != nullptr) {
            return foundation::Result<Entry>::success(*compatible);
        }
    }
    return foundation::Result<Entry>::failure(queryError(
        "Query.UnsupportedVersion",
        foundation::ErrorCategory::Validation,
        "The requested query version is not supported",
        key));
}

void QueryRegistry::freeze()
{
    std::unique_lock lock(mutex_);
    frozen_ = true;
}

} // namespace lasercnc::runtime
