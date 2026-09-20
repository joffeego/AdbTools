// Tests for core/batch.h.
//
// This is the accounting the GUI and the CLI both rely on, and it exists because
// of a real bug: a batch used to give up on the first failure, silently skipping
// everything after it. The property that pins that down is `complete()` - a
// finished batch must have attempted every item - so most of these cases assert
// the counters stay consistent with the recorded items.
#include "core/batch.h"

#include <string>
#include <vector>

#include "tests/test_main.h"

using namespace adb::core;

ADB_TEST(batch_starts_empty) {
    BatchOutcome outcome({"a", "b", "c"});
    ADB_CHECK_EQ(outcome.total(), static_cast<std::size_t>(3));
    ADB_CHECK_EQ(outcome.attempted(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(outcome.succeeded(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(outcome.failed(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(outcome.unattempted(), static_cast<std::size_t>(3));
    ADB_CHECK(!outcome.complete());
}

ADB_TEST(batch_all_succeed) {
    BatchOutcome outcome({"a", "b"});
    outcome.record(0, true);
    outcome.record(1, true);
    ADB_CHECK(outcome.complete());
    ADB_CHECK_EQ(outcome.succeeded(), static_cast<std::size_t>(2));
    ADB_CHECK_EQ(outcome.failed(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(outcome.summaryLine(), std::string("全部成功（2 项）"));
    ADB_CHECK_EQ(outcome.toastTitle("删除完成"), std::string("删除完成"));
    ADB_CHECK(outcome.failedLabels().empty());
}

ADB_TEST(batch_partial_failure_is_counted) {
    BatchOutcome outcome({"ok1", "bad", "ok2"});
    outcome.record(0, true);
    outcome.record(1, false, "adb: error: nope");
    outcome.record(2, true);
    // The whole point: the item after the failure was still attempted.
    ADB_CHECK(outcome.complete());
    ADB_CHECK_EQ(outcome.succeeded(), static_cast<std::size_t>(2));
    ADB_CHECK_EQ(outcome.failed(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(outcome.summaryLine(), std::string("成功 2，失败 1"));
    ADB_CHECK_EQ(outcome.toastTitle("删除完成"), std::string("删除完成（有失败）"));

    const std::vector<std::string> labels = outcome.failedLabels();
    ADB_CHECK_EQ(labels.size(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(ADB_AT(labels, 0), std::string("bad"));
}

ADB_TEST(batch_detects_early_stop) {
    // A batch that stopped after the first failure: this is the state the old
    // implementation left behind (and never reported).
    BatchOutcome outcome({"a", "b", "c"});
    outcome.record(0, false, "boom");
    ADB_CHECK(!outcome.complete());
    ADB_CHECK_EQ(outcome.unattempted(), static_cast<std::size_t>(2));
    // The report must say so rather than looking like a 1-item batch.
    ADB_CHECK(outcome.text().find("未处理 2 项") != std::string::npos);
}

ADB_TEST(batch_text_names_failures_with_errors) {
    BatchOutcome outcome({"one", "two", "three", "four"});
    outcome.record(0, true);
    outcome.record(1, false, "first error");
    outcome.record(2, false, "second error");
    outcome.record(3, false, "third error");

    const std::string text = outcome.text();
    ADB_CHECK(text.find("成功 1，失败 3") != std::string::npos);
    // Named failures carry their error text, because "which one" is not enough
    // to act on.
    ADB_CHECK(text.find("first error") != std::string::npos);
    ADB_CHECK(text.find("third error") != std::string::npos);
}

ADB_TEST(batch_text_truncates_a_long_failure_list) {
    std::vector<std::string> labels;
    for (int i = 0; i < 6; ++i) labels.push_back("item" + std::to_string(i));
    BatchOutcome outcome(labels);
    for (std::size_t i = 0; i < labels.size(); ++i) outcome.record(i, false, "e");

    const std::string text = outcome.text(2);  // name at most two
    ADB_CHECK(text.find("item0") != std::string::npos);
    ADB_CHECK(text.find("item1") != std::string::npos);
    // The rest are summarised rather than omitted silently.
    ADB_CHECK(text.find("等 6 项") != std::string::npos);
}

ADB_TEST(batch_long_labels_and_errors_are_shortened) {
    const std::string longName(300, 'x');
    BatchOutcome outcome({longName});
    outcome.record(0, false, std::string(300, 'y'));
    const std::string text = outcome.text();
    // A 300-character name would otherwise fill the toast.
    ADB_CHECK(text.find("...") != std::string::npos);
    ADB_CHECK(text.size() < 200);
}

ADB_TEST(batch_re_recording_an_item_does_not_double_count) {
    // A retry (or a callback delivered twice) must correct the previous verdict,
    // not add to it - otherwise the totals stop matching the item list.
    BatchOutcome outcome({"a"});
    outcome.record(0, false, "first attempt failed");
    outcome.record(0, true);
    ADB_CHECK_EQ(outcome.attempted(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(outcome.succeeded(), static_cast<std::size_t>(1));
    ADB_CHECK_EQ(outcome.failed(), static_cast<std::size_t>(0));
    ADB_CHECK(outcome.failedLabels().empty());
}

ADB_TEST(batch_out_of_range_index_is_ignored) {
    BatchOutcome outcome({"a"});
    outcome.record(5, true);
    ADB_CHECK_EQ(outcome.attempted(), static_cast<std::size_t>(0));
    ADB_CHECK_EQ(outcome.labelAt(5), std::string(""));
}

ADB_TEST(batch_empty) {
    BatchOutcome outcome;
    ADB_CHECK_EQ(outcome.total(), static_cast<std::size_t>(0));
    ADB_CHECK(outcome.complete());  // vacuously: nothing to do
    ADB_CHECK_EQ(outcome.summaryLine(), std::string("全部成功（0 项）"));
}

ADB_TEST(batch_json_reports_totals_and_failures) {
    BatchOutcome outcome({"good", "bad"});
    outcome.record(0, true);
    outcome.record(1, false, "some error");
    const std::string json = outcome.toJson();
    ADB_CHECK(json.find("\"total\":2") != std::string::npos);
    ADB_CHECK(json.find("\"succeeded\":1") != std::string::npos);
    ADB_CHECK(json.find("\"failed\":1") != std::string::npos);
    ADB_CHECK(json.find("\"label\":\"bad\"") != std::string::npos);
    ADB_CHECK(json.find("\"error\":\"some error\"") != std::string::npos);
}

ADB_TEST(batch_json_escapes_control_characters) {
    // Error text comes from adb and can contain quotes, newlines and tabs; the
    // CLI feeds this straight to a script, so it has to stay valid JSON.
    BatchOutcome outcome({"bad"});
    outcome.record(0, false, "quote \" backslash \\ newline \n tab \t");
    const std::string json = outcome.toJson();
    ADB_CHECK(json.find("\\\"") != std::string::npos);
    ADB_CHECK(json.find("\\\\") != std::string::npos);
    ADB_CHECK(json.find("\\n") != std::string::npos);
    ADB_CHECK(json.find("\\t") != std::string::npos);
    // No raw newline may survive inside the JSON.
    ADB_CHECK(json.find('\n') == std::string::npos);
}

ADB_TEST(batch_json_is_empty_array_when_nothing_failed) {
    BatchOutcome outcome({"a", "b"});
    outcome.record(0, true);
    outcome.record(1, true);
    const std::string json = outcome.toJson();
    ADB_CHECK(json.find("\"failures\":[]") != std::string::npos);
}

ADB_TEST(batch_label_from_construction_is_kept) {
    BatchOutcome outcome({"/sdcard/a b.txt", "中文名.txt"});
    ADB_CHECK_EQ(outcome.labelAt(0), std::string("/sdcard/a b.txt"));
    ADB_CHECK_EQ(outcome.labelAt(1), std::string("中文名.txt"));
}
