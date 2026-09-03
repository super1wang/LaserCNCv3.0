#include <lasercnc/foundation/version.hpp>

namespace lasercnc::foundation {

std::string Version::toString() const
{
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
}

} // namespace lasercnc::foundation
