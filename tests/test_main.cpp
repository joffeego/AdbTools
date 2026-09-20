// Entry point for the unit tests. Each test_*.cpp registers its cases through
// the ADB_TEST macro; this file only runs them.
#include "tests/test_main.h"

int main() {
    return adb::test::runAll();
}
