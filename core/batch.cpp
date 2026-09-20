#include "core/batch.h"

#include <cstdio>

#include "core/strings.h"

namespace adb::core {

namespace {

// Shared by text() and toJson(): the failures, newest-last, as the user sees
// them.
std::vector<const BatchItem*> failuresOf(const std::vector<BatchItem>& items) {
    std::vector<const BatchItem*> out;
    for (const BatchItem& item : items) {
        if (item.attempted && !item.ok) out.push_back(&item);
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

BatchOutcome::BatchOutcome(std::vector<std::string> labels) {
    items_.reserve(labels.size());
    for (std::string& label : labels) {
        BatchItem item;
        item.label = std::move(label);
        items_.push_back(std::move(item));
    }
}

const std::string& BatchOutcome::labelAt(std::size_t index) const {
    static const std::string empty;
    if (index >= items_.size()) return empty;
    return items_[index].label;
}

void BatchOutcome::record(std::size_t index, bool ok, const std::string& error) {
    if (index >= items_.size()) return;  // out of range: nothing to record
    BatchItem& item = items_[index];
    if (!item.attempted) {
        item.attempted = true;
        ++attempted_;
    } else {
        // Re-recording an item (a retry, or a double callback) must not
        // double-count: correct the previous verdict first.
        if (item.ok) --succeeded_;
        else --failed_;
    }
    item.ok = ok;
    item.error = ok ? std::string() : error;
    if (ok) ++succeeded_;
    else ++failed_;
}

std::vector<std::string> BatchOutcome::failedLabels() const {
    std::vector<std::string> out;
    for (const BatchItem* item : failuresOf(items_)) out.push_back(item->label);
    return out;
}

std::string BatchOutcome::summaryLine() const {
    if (failed_ == 0) {
        // Say the total as well: "全部成功" alone leaves the reader unsure how
        // many that was.
        return "全部成功（" + std::to_string(succeeded_) + " 项）";
    }
    return "成功 " + std::to_string(succeeded_) + "，失败 " + std::to_string(failed_);
}

std::string BatchOutcome::text(std::size_t maxNamed) const {
    const std::vector<const BatchItem*> failures = failuresOf(items_);
    std::string out = summaryLine();
    if (!failures.empty()) {
        out += "：";
        const std::size_t shown = failures.size() < maxNamed ? failures.size() : maxNamed;
        for (std::size_t i = 0; i < shown; ++i) {
            if (i) out += "；";
            out += "\"" + shorten(failures[i]->label, 28) + "\"";
            if (!failures[i]->error.empty()) out += "（" + shorten(failures[i]->error, 60) + "）";
        }
        if (failures.size() > shown) {
            out += " 等 " + std::to_string(failures.size()) + " 项";
        }
    }
    if (unattempted() > 0) {
        // Should be impossible for a finished batch; stated explicitly so a
        // regression that reintroduces early exit is reported rather than
        // silently looking like success.
        out += "\n未处理 " + std::to_string(unattempted()) + " 项";
    }
    return out;
}

std::string BatchOutcome::toJson() const {
    std::string out = "{\"total\":" + std::to_string(total()) +
                      ",\"attempted\":" + std::to_string(attempted_) +
                      ",\"succeeded\":" + std::to_string(succeeded_) +
                      ",\"failed\":" + std::to_string(failed_) + ",\"failures\":[";
    bool first = true;
    for (const BatchItem* item : failuresOf(items_)) {
        if (!first) out += ",";
        first = false;
        out += "{\"label\":\"" + jsonEscape(item->label) + "\",\"error\":\"" +
               jsonEscape(item->error) + "\"}";
    }
    out += "]}";
    return out;
}

std::string BatchOutcome::toastTitle(const std::string& successTitle) const {
    if (failed_ == 0 && unattempted() == 0) return successTitle;
    return successTitle + "（有失败）";
}

}  // namespace adb::core
