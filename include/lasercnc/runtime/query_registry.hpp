#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/runtime/query.hpp>

#include <map>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace lasercnc::kernel {
class AppKernel;
}

namespace lasercnc::runtime {

class QueryRuntime;

class QueryRegistry final {
public:
    [[nodiscard]] foundation::Result<void> registerHandler(
        QueryDescriptor descriptor,
        std::shared_ptr<IQueryHandler> handler);
    [[nodiscard]] foundation::Result<QueryDescriptor> descriptor(
        const kernel::QueryName& name) const;
    [[nodiscard]] std::vector<QueryDescriptor> descriptors() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool frozen() const;

private:
    friend class QueryRuntime;
    friend class kernel::AppKernel;

    struct Entry final {
        QueryDescriptor descriptor;
        std::shared_ptr<IQueryHandler> handler;
    };

    [[nodiscard]] foundation::Result<Entry> resolve(const kernel::QueryName& name) const;
    void freeze();

    mutable std::shared_mutex mutex_;
    std::map<kernel::QueryName, Entry> entries_;
    bool frozen_{false};
};

} // namespace lasercnc::runtime
