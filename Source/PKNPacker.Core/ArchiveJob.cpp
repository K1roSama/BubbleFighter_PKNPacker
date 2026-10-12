#include "ArchiveJob.h"
#include <algorithm>
#include <thread>

namespace PKNPacker {
void ArchiveJob::Check() const {
    if (cancelled && cancelled->load()) throw ArchiveError(ErrorCode::Cancelled, "任务已取消");
}
void ArchiveJob::Report(std::string stage, std::string item, std::uint64_t done, std::uint64_t total) const {
    Check();
    if (progress) progress({std::move(stage), std::move(item), done, total});
}
bool JobResult::Succeeded() const {
    return std::all_of(items.begin(), items.end(), [](const auto& item) {
        return item.status == ItemStatus::Success || item.status == ItemStatus::Skipped;
    });
}
JobResult RunArchiveJobs(const std::vector<std::pair<std::string, std::function<void()>>>& tasks,
                        const ArchiveJob& job, unsigned concurrency) {
    JobResult result;
    result.items.resize(tasks.size());
    std::atomic_size_t next = 0;
    std::vector<std::jthread> workers;
    for (unsigned n = 0; n < std::min<unsigned>(std::clamp(concurrency, 1U, 8U), static_cast<unsigned>(tasks.size())); ++n) {
        workers.emplace_back([&] {
            for (;;) {
                const auto i = next.fetch_add(1);
                if (i >= tasks.size()) return;
                auto& item = result.items[i];
                item.item = tasks[i].first;
                try {
                    job.Check();
                    tasks[i].second();
                } catch (const ArchiveError& error) {
                    item.status = error.Code() == ErrorCode::Cancelled ? ItemStatus::Cancelled : ItemStatus::Failed;
                    item.message = error.what();
                } catch (const std::exception& error) {
                    item.status = ItemStatus::Failed;
                    item.message = error.what();
                }
            }
        });
    }
    workers.clear(); // Join before returning or moving the result.
    return result;
}
}
