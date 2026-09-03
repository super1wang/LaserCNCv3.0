#pragma once

#include <lasercnc/foundation/result.hpp>
#include <lasercnc/foundation/value.hpp>
#include <lasercnc/kernel/identifiers.hpp>

#include <cstddef>
#include <map>
#include <vector>

namespace lasercnc::runtime {
class ApplicationTransaction;
class TransactionManager;
}

namespace lasercnc::state {

struct ObjectRecord final {
    kernel::ObjectId id;
    kernel::ObjectTypeId type;
    foundation::Value data;

    friend bool operator==(const ObjectRecord&, const ObjectRecord&) = default;
};

class ObjectRegistry final {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool contains(const kernel::ObjectId& id) const noexcept;
    [[nodiscard]] const ObjectRecord* find(const kernel::ObjectId& id) const noexcept;
    [[nodiscard]] std::vector<ObjectRecord> all() const;

private:
    friend class runtime::ApplicationTransaction;
    friend class runtime::TransactionManager;

    [[nodiscard]] foundation::Result<void> insert(ObjectRecord object);
    [[nodiscard]] foundation::Result<void> replaceData(
        const kernel::ObjectId& id,
        foundation::Value data);
    [[nodiscard]] foundation::Result<void> erase(const kernel::ObjectId& id);
    void swap(ObjectRegistry& other) noexcept;

    std::map<kernel::ObjectId, ObjectRecord> objects_;
};

} // namespace lasercnc::state
