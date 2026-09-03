#include <lasercnc/runtime/asset_validation.hpp>

#include <lasercnc/foundation/error.hpp>

#include <map>
#include <set>
#include <string>

namespace lasercnc::runtime {

foundation::Result<void> validateObjectAssets(
    std::span<const state::ObjectRecord> records, const platform::IAssetStore* store)
{
    std::map<kernel::AssetId, state::AssetRef> verified;
    for(const auto& record : records) {
        std::set<kernel::AssetId> local;
        for(const auto& reference : record.assets) {
            const auto error = [&](const char* code, const char* message,
                                   std::shared_ptr<const foundation::Error> cause = nullptr) {
                return foundation::Result<void>::failure(foundation::makeError(
                    code, foundation::ErrorCategory::Validation, message,
                    foundation::Value {foundation::Value::Object {
                        {"objectId", foundation::Value {std::string(record.id.value())}},
                        {"assetId", foundation::Value {std::string(reference.id.value())}},
                    }}, foundation::Severity::Error, std::move(cause)));
            };
            if(store == nullptr) {
                return error("Asset.StoreRequired", "Objects with asset references require a configured asset store");
            }
            if(!local.insert(reference.id).second) {
                return error("Asset.DuplicateReference", "An object cannot list an asset identity more than once");
            }
            const auto prior = verified.find(reference.id);
            if(prior != verified.end()) {
                if(prior->second != reference) {
                    return error("Asset.ReferenceConflict", "One asset identity has conflicting metadata in the candidate state");
                }
                continue;
            }
            try {
                auto checked = store->verify(reference);
                if(!checked) {
                    return error("Asset.StateAdmissionFailed", "An object asset is missing, corrupt or inconsistent",
                        std::make_shared<const foundation::Error>(std::move(checked).error()));
                }
            } catch(...) {
                return error("Asset.StateAdmissionException", "Asset verification raised an exception during state admission");
            }
            verified.emplace(reference.id, reference);
        }
    }
    return foundation::Result<void>::success();
}

} // namespace lasercnc::runtime
