#pragma once
#include <filesystem>
#include <functional>
void setSnapshotCreateProbe(std::function<void(const std::filesystem::path&)> probe);
void setSnapshotPublishProbe(std::function<void(const std::filesystem::path&)> probe);
void setSnapshotReadSharingFailures(unsigned int count);
unsigned int snapshotReadAttempts();
