#pragma once

#include <filesystem>
#include <string_view>

int runKernelCrashContract(std::string_view mode, const std::filesystem::path& root, std::string_view scenario);
