// Command-line interface.
//
// One executable serves both modes: no arguments opens the GUI, arguments run a
// command and exit. See cli/README.md for the command list and conventions.
#pragma once

#include <string>
#include <vector>

namespace adb::cli {

// True when the given argv should be handled as a CLI invocation rather than
// starting the GUI. `argv[0]` (the program path) is not inspected.
bool wantsCli(const std::vector<std::string>& args);

// Run a command. Returns a process exit code:
//   0  success
//   1  the operation failed (adb error, missing file, ...)
//   2  usage error (unknown command, missing/extra argument)
int run(const std::vector<std::string>& args);

}  // namespace adb::cli
