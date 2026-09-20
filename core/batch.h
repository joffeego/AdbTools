// Accounting for batch operations: pushing, pulling or deleting many items at
// once.
//
// The GUI and the CLI both run batches, and both used to keep the tally in
// ad-hoc locals threaded through their own loop (a running `ok` counter plus a
// `std::vector<std::string> failures` passed along by value). That worked, but
// it meant the same three things were written twice and could drift:
//
//   * how an item's outcome is recorded
//   * how the summary is worded
//   * how "not attempted yet" is represented (it was not: the counters just
//     stopped advancing, which is how the original bug - a batch that aborted on
//     the first failure and silently skipped the rest - stayed invisible)
//
// BatchOutcome models that state explicitly. `attempted()` must equal `total()`
// at the end of a run, so a batch that stops early is now detectable rather than
// merely unlikely.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace adb::core {

// One item of a batch and what happened to it.
struct BatchItem {
    std::string label;       // shown to the user: a file name or a remote path
    bool attempted = false;  // false means the batch ended before reaching it
    bool ok = false;
    std::string error;       // error text when !ok
};

// Tracks a batch from start to finish.
//
// Usage:
//     BatchOutcome outcome(items);
//     ...
//     outcome.record(index, true);
//     outcome.record(index, false, "adb: error: ...");
//     ...
//     outcome.summaryLine();      // "成功 3，失败 1"
//     outcome.text();             // multi-line report naming the failures
class BatchOutcome {
public:
    BatchOutcome() = default;
    explicit BatchOutcome(std::vector<std::string> labels);

    std::size_t total() const { return items_.size(); }
    std::size_t attempted() const { return attempted_; }
    std::size_t succeeded() const { return succeeded_; }
    std::size_t failed() const { return failed_; }

    // True when every item has been attempted - the state a finished batch must
    // be in.
    bool complete() const { return attempted_ == items_.size(); }

    // Items that were never attempted. Non-empty only when the batch stopped
    // early, i.e. when something is wrong.
    std::size_t unattempted() const { return items_.size() - attempted_; }

    // Label of the item at `index` ("" when out of range).
    const std::string& labelAt(std::size_t index) const;

    // Record the outcome of one item. `error` is kept only for failures.
    void record(std::size_t index, bool ok, const std::string& error = std::string());

    const std::vector<BatchItem>& items() const { return items_; }

    // Labels of the items that failed, in completion order.
    std::vector<std::string> failedLabels() const;

    // --- reporting ----------------------------------------------------------

    // "成功 3，失败 1"，or "全部成功（3 项）" when nothing failed.
    std::string summaryLine() const;

    // Full human-readable report: the summary line, then up to `maxNamed`
    // failures with their error text. Matches what the GUI shows in a toast.
    std::string text(std::size_t maxNamed = 3) const;

    // JSON object for scripting: totals plus a failures array (label + error).
    // The CLI uses this so `--json` needs no serialisation of its own.
    std::string toJson() const;

    // The GUI shows a different title depending on whether anything failed.
    std::string toastTitle(const std::string& successTitle) const;

private:
    std::vector<BatchItem> items_;
    std::size_t attempted_ = 0;
    std::size_t succeeded_ = 0;
    std::size_t failed_ = 0;
};

}  // namespace adb::core
