#include <lasercnc/infrastructure/filesystem_asset_store.hpp>

#include <lasercnc/foundation/error.hpp>
#include <lasercnc/infrastructure/filesystem_snapshot_store.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace lasercnc::infrastructure {
namespace {

constexpr std::string_view magic = "LCNCAS01";
constexpr std::size_t maximumKindBytes = 256U;
constexpr std::size_t maximumHeaderBytes = magic.size() + 4U + maximumKindBytes;

foundation::Error assetError(const char* code, const char* message,
                             std::shared_ptr<const foundation::Error> cause = nullptr)
{
    return foundation::makeError(code, foundation::ErrorCategory::Infrastructure, message,
        foundation::Value{}, foundation::Severity::Error, std::move(cause));
}

std::span<const std::byte> bytes(std::string_view value) noexcept
{
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

bool validDigest(std::string_view digest) noexcept
{
    return digest.size() == 71U && digest.starts_with("sha256:")
        && std::all_of(digest.begin() + 7, digest.end(), [](char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        });
}

foundation::Result<kernel::ContentDigest> digestChecked(
    const platform::IHashService& hashes, std::span<const std::byte> content)
{
    auto digest = hashes.digest(content);
    if(!digest) {
        return foundation::Result<kernel::ContentDigest>::failure(assetError(
            "Asset.HashFailed", "Asset hashing failed",
            std::make_shared<const foundation::Error>(std::move(digest).error())));
    }
    if(!validDigest(digest.value().value())) {
        return foundation::Result<kernel::ContentDigest>::failure(assetError(
            "Asset.InvalidDigest", "The filesystem asset store requires canonical SHA-256 digests"));
    }
    return digest;
}

foundation::Result<kernel::AssetId> deriveId(
    const platform::IHashService& hashes, const kernel::AssetKind& kind,
    const kernel::ContentDigest& digest, std::uint64_t size)
{
    const std::string identity = "LCNCAssetRef1\n" + std::to_string(kind.value().size()) + ':'
        + std::string(kind.value()) + '\n' + std::string(digest.value()) + ':' + std::to_string(size);
    auto hashed = digestChecked(hashes, bytes(identity));
    if(!hashed) {
        return foundation::Result<kernel::AssetId>::failure(std::move(hashed).error());
    }
    return kernel::AssetId::create("asset.sha256." + std::string(hashed.value().value().substr(7U)));
}

std::string envelope(const kernel::AssetKind& kind, std::span<const std::byte> content)
{
    std::string result(magic);
    result.reserve(magic.size() + 4U + kind.value().size() + content.size());
    const auto length = static_cast<std::uint32_t>(kind.value().size());
    for(const auto shift : {24U, 16U, 8U, 0U}) {
        result.push_back(static_cast<char>((length >> shift) & 0xffU));
    }
    result.append(kind.value());
    if(!content.empty()) {
        result.append(reinterpret_cast<const char*>(content.data()), content.size());
    }
    return result;
}

struct VerifiedContent final {
    std::string envelope;
    std::size_t offset;
};

} // namespace

class FilesystemAssetStore::Impl final {
public:
    Impl(std::unique_ptr<FilesystemSnapshotStore> files,
         std::shared_ptr<const platform::IHashService> hashes, std::size_t limit)
        : files_(std::move(files)), hashes_(std::move(hashes)), limit_(limit) {}

    foundation::Result<VerifiedContent> verified(const state::AssetRef& reference) const
    {
        if(reference.kind.value().size() > maximumKindBytes || reference.byteSize > limit_
           || !validDigest(reference.digest.value())) {
            return foundation::Result<VerifiedContent>::failure(assetError(
                "Asset.InvalidReference", "Asset reference metadata is invalid or exceeds store limits"));
        }
        auto identity = deriveId(*hashes_, reference.kind, reference.digest, reference.byteSize);
        if(!identity) {
            return foundation::Result<VerifiedContent>::failure(std::move(identity).error());
        }
        if(identity.value() != reference.id) {
            return foundation::Result<VerifiedContent>::failure(assetError(
                "Asset.IdentityMismatch", "Asset identity does not bind its kind, digest and size"));
        }
        auto key = kernel::SnapshotId::create(std::string(reference.id.value()));
        if(!key) {
            return foundation::Result<VerifiedContent>::failure(std::move(key).error());
        }
        auto stored = files_->read(key.value());
        if(!stored) {
            const bool missing = stored.error().category == foundation::ErrorCategory::NotFound;
            return foundation::Result<VerifiedContent>::failure(assetError(
                missing ? "Asset.NotFound" : "Asset.ReadFailed", "The immutable asset could not be read",
                std::make_shared<const foundation::Error>(std::move(stored).error())));
        }
        const auto& payload = stored.value();
        if(payload.size() < magic.size() + 4U || !payload.starts_with(magic)) {
            return foundation::Result<VerifiedContent>::failure(assetError(
                "Asset.InvalidEnvelope", "The immutable asset envelope is truncated or unsupported"));
        }
        std::uint32_t length = 0U;
        for(std::size_t index = 0U; index < 4U; ++index) {
            length = (length << 8U) | static_cast<unsigned char>(payload[magic.size() + index]);
        }
        if(length > maximumKindBytes || payload.size() < magic.size() + 4U + length) {
            return foundation::Result<VerifiedContent>::failure(assetError(
                "Asset.InvalidEnvelope", "The immutable asset kind header is invalid"));
        }
        const auto offset = magic.size() + 4U + length;
        if(std::string_view(payload).substr(magic.size() + 4U, length) != reference.kind.value()
           || payload.size() - offset != reference.byteSize) {
            return foundation::Result<VerifiedContent>::failure(assetError(
                "Asset.MetadataMismatch", "The immutable asset does not match its declared kind or size"));
        }
        auto actual = digestChecked(*hashes_, bytes(payload).subspan(offset));
        if(!actual) {
            return foundation::Result<VerifiedContent>::failure(std::move(actual).error());
        }
        if(actual.value() != reference.digest) {
            return foundation::Result<VerifiedContent>::failure(assetError(
                "Asset.DigestMismatch", "The immutable asset content failed its digest check"));
        }
        return foundation::Result<VerifiedContent>::success({std::move(stored).value(), offset});
    }

    std::unique_ptr<FilesystemSnapshotStore> files_;
    std::shared_ptr<const platform::IHashService> hashes_;
    std::size_t limit_;
};

FilesystemAssetStore::FilesystemAssetStore(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

FilesystemAssetStore::~FilesystemAssetStore() = default;

foundation::Result<std::unique_ptr<FilesystemAssetStore>> FilesystemAssetStore::create(
    FilesystemAssetStoreOptions options, std::shared_ptr<const platform::IHashService> hashes)
{
    try {
        if(hashes == nullptr || options.maximumContentBytes == 0U
           || options.maximumContentBytes > std::numeric_limits<std::size_t>::max() - maximumHeaderBytes) {
            return foundation::Result<std::unique_ptr<FilesystemAssetStore>>::failure(assetError(
                "Asset.InvalidStoreOptions", "Asset store requires a hash service and bounded positive content limit"));
        }
        auto files = FilesystemSnapshotStore::create({std::move(options.directory), options.maximumContentBytes + maximumHeaderBytes});
        if(!files) {
            return foundation::Result<std::unique_ptr<FilesystemAssetStore>>::failure(assetError(
                "Asset.StoreInitializationFailed", "Asset file storage could not be initialized",
                std::make_shared<const foundation::Error>(std::move(files).error())));
        }
        return foundation::Result<std::unique_ptr<FilesystemAssetStore>>::success(
            std::unique_ptr<FilesystemAssetStore>(new FilesystemAssetStore(std::make_unique<Impl>(
                std::move(files).value(), std::move(hashes), options.maximumContentBytes))));
    } catch(...) {
        return foundation::Result<std::unique_ptr<FilesystemAssetStore>>::failure(assetError(
            "Asset.StoreInitializationException", "Asset store initialization raised an exception"));
    }
}

foundation::Result<state::AssetRef> FilesystemAssetStore::publish(
    const kernel::AssetKind& kind, std::span<const std::byte> content)
{
    try {
        if(kind.value().size() > maximumKindBytes || content.size() > implementation_->limit_) {
            return foundation::Result<state::AssetRef>::failure(assetError(
                "Asset.PayloadTooLarge", "Asset kind or content exceeds configured limits"));
        }
        auto digest = digestChecked(*implementation_->hashes_, content);
        if(!digest) {
            return foundation::Result<state::AssetRef>::failure(std::move(digest).error());
        }
        auto id = deriveId(*implementation_->hashes_, kind, digest.value(), content.size());
        if(!id) {
            return foundation::Result<state::AssetRef>::failure(std::move(id).error());
        }
        state::AssetRef reference{std::move(id).value(), std::move(digest).value(), kind,
            static_cast<std::uint64_t>(content.size())};
        auto key = kernel::SnapshotId::create(std::string(reference.id.value()));
        if(!key) {
            return foundation::Result<state::AssetRef>::failure(std::move(key).error());
        }
        auto written = implementation_->files_->writeAtomically(key.value(), envelope(kind, content));
        if(!written) {
            return foundation::Result<state::AssetRef>::failure(assetError(
                "Asset.PublishFailed", "Asset content could not be published immutably",
                std::make_shared<const foundation::Error>(std::move(written).error())));
        }
        auto checked = verify(reference);
        if(!checked) {
            return foundation::Result<state::AssetRef>::failure(std::move(checked).error());
        }
        return foundation::Result<state::AssetRef>::success(std::move(reference));
    } catch(...) {
        return foundation::Result<state::AssetRef>::failure(assetError(
            "Asset.PublishException", "Asset publication raised an exception; an unreferenced asset may remain"));
    }
}

foundation::Result<std::vector<std::byte>> FilesystemAssetStore::read(const state::AssetRef& reference) const
{
    try {
        auto checked = implementation_->verified(reference);
        if(!checked) {
            return foundation::Result<std::vector<std::byte>>::failure(std::move(checked).error());
        }
        const auto content = bytes(checked.value().envelope).subspan(checked.value().offset);
        return foundation::Result<std::vector<std::byte>>::success({content.begin(), content.end()});
    } catch(...) {
        return foundation::Result<std::vector<std::byte>>::failure(assetError(
            "Asset.ReadException", "Asset reading raised an exception"));
    }
}

foundation::Result<void> FilesystemAssetStore::verify(const state::AssetRef& reference) const
{
    try {
        auto checked = implementation_->verified(reference);
        return checked ? foundation::Result<void>::success()
                       : foundation::Result<void>::failure(std::move(checked).error());
    } catch(...) {
        return foundation::Result<void>::failure(assetError(
            "Asset.VerifyException", "Asset verification raised an exception"));
    }
}

} // namespace lasercnc::infrastructure
