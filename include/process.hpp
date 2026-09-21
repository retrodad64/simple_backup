#pragma once
#include <string>
#include <vector>

namespace Process {

// Runs a command and waits for it. No shell is involved, so an argument
// containing spaces needs no quoting. Returns the exit status, or -1 when the
// command could not be run at all. stdout is discarded; stderr is left alone
// so diagnostics reach the terminal or the journal.
int run(const std::vector<std::string>& argv);

}
