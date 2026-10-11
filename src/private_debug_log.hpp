#pragma once

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace hymission {

// Keep untrusted metadata on one line in both the compositor and private log.
inline std::string escapeLogControls(std::string_view message) {
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(message.size());
    for (std::size_t i = 0; i < message.size(); ++i) {
        const auto c = static_cast<unsigned char>(message[i]);
        if (c < 0x20 || c == 0x7f) {
            escaped += "\\x";
            escaped += hex[c >> 4];
            escaped += hex[c & 15];
        } else if (c == 0xc2 && i + 1 < message.size() &&
                   static_cast<unsigned char>(message[i + 1]) >= 0x80 && static_cast<unsigned char>(message[i + 1]) <= 0x9f) {
            const auto control = static_cast<unsigned char>(message[++i]);
            escaped += "\\u00";
            escaped += hex[control >> 4];
            escaped += hex[control & 15];
        } else if (c == 0xe2 && i + 2 < message.size() && static_cast<unsigned char>(message[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(message[i + 2]) == 0xa8 || static_cast<unsigned char>(message[i + 2]) == 0xa9)) {
            escaped += static_cast<unsigned char>(message[i + 2]) == 0xa8 ? "\\u2028" : "\\u2029";
            i += 2;
        } else if (c >= 0xc2 && c <= 0xf4) {
            const std::size_t length = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            bool valid = i + length <= message.size();
            for (std::size_t offset = 1; valid && offset < length; ++offset)
                valid = (static_cast<unsigned char>(message[i + offset]) & 0xc0) == 0x80;
            if (valid) {
                escaped.append(message.substr(i, length));
                i += length - 1;
            } else
                escaped += message[i];
        } else if (c >= 0x80 && c <= 0x9f) {
            escaped += "\\x";
            escaped += hex[c >> 4];
            escaped += hex[c & 15];
        } else
            escaped += message[i];
    }
    return escaped;
}

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
    const auto line = escapeLogControls(message) + '\n';
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
