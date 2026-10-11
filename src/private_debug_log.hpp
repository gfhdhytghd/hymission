#pragma once

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace hymission {

// Fail closed unless the runtime directory and log belong exclusively to us.
inline void appendPrivateDebugLog(const std::string& message) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime[0] != '/')
        return;
    const int dir = open(runtime, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0)
        return;
    struct stat info{};
    if (fstat(dir, &info) != 0 || info.st_uid != geteuid() || (info.st_mode & 0077) != 0) {
        close(dir);
        return;
    }
    const int fd = openat(dir, "hymission-debug.log", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    close(dir);
    if (fd < 0)
        return;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || fchmod(fd, 0600) != 0) {
        close(fd);
        return;
    }
    const auto line = message + '\n';
    std::size_t offset = 0;
    while (offset < line.size()) {
        const auto written = write(fd, line.data() + offset, line.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        offset += static_cast<std::size_t>(written);
    }
    close(fd);
}

} // namespace hymission
