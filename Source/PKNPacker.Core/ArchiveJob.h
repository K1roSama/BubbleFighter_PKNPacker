#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace PKNPacker {
enum class ErrorCode { InvalidFormat, Bounds, Unsupported, Conflict, Io, Cancelled, Integrity };
class ArchiveError : public std::runtime_error {
public:
    ArchiveError(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
    ErrorCode Code() const noexcept { return code_; }
private:
    ErrorCode code_;
};
struct JobProgress {
    std::string stage;
    std::string item;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
};
struct ArchiveJob {
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    std::function<void(const JobProgress&)> progress;
    void Check() const;
    void Report(std::string stage, std::string item, std::uint64_t done, std::uint64_t total) const;
};
enum class ItemStatus { Success, Failed, Skipped, Cancelled };
struct ItemResult {
    std::string item;
    ItemStatus status = ItemStatus::Success;
    std::string message;
};
struct JobResult {
    std::vector<ItemResult> items;
    bool Succeeded() const;
};
// Runs at most two independent archives by default. Each operation owns its writer.
JobResult RunArchiveJobs(const std::vector<std::pair<std::string, std::function<void()>>>& tasks,
                         const ArchiveJob& job = {}, unsigned concurrency = 2);
}
