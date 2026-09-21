#include "process.hpp"
#include "logger.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace Process {

int run(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return -1;
    }

    std::vector<char*> raw;
    for (const auto& argument : argv) {
        raw.push_back(const_cast<char*>(argument.c_str()));
    }

    raw.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        Logger::warn(std::string("fork: ") + strerror(errno));
        return -1;
    }

    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }

        execvp(raw[0], raw.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}
