#pragma once
// Writes the fuzz input to a scratch file under $TMPDIR and returns its path; removed on destruction.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace fuzz {

class TempFile {
public:
    TempFile(const uint8_t* data, size_t size, const char* suffix) {
        const char* dir = getenv("TMPDIR");
        std::string tmpl = std::string(dir && *dir ? dir : "/tmp") + "/lbug_fuzz_XXXXXX" + suffix;
        int fd = mkstemps(tmpl.data(), static_cast<int>(std::char_traits<char>::length(suffix)));
        if (fd < 0) {
            abort();
        }
        size_t off = 0;
        while (off < size) {
            ssize_t n = write(fd, data + off, size - off);
            if (n <= 0) {
                break;
            }
            off += static_cast<size_t>(n);
        }
        close(fd);
        path_ = tmpl;
    }
    ~TempFile() { unlink(path_.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

} // namespace fuzz
