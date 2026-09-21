#pragma once

namespace Configurator {

// Walks the user through building a config file and writes it out. Returns a
// process exit code: 0 when a file was written, 1 when the user aborted or
// something failed.
int run();

}
