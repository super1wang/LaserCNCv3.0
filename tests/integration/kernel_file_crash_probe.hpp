#pragma once

#include <filesystem>
#include <functional>

using KernelFilePublishProbe = std::function<void(const std::filesystem::path&, const std::filesystem::path&, bool)>;
void setKernelFilePublishProbe(KernelFilePublishProbe probe);
