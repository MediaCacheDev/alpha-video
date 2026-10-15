#include "ByteSource.h"

#include "GrowingFile.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace webm {

namespace {

class FileRangeSource : public ByteSource {
public:
    // Takes ownership of fd. `download` is set while the file is still downloading; `length`
    // is unused then.
    FileRangeSource(int fd, int64_t offset, int64_t length, std::shared_ptr<GrowingFile> download,
                    const std::atomic<bool>* abort)
        : fd_(fd), offset_(offset), length_(length), download_(std::move(download)), abort_(abort) {}

    ~FileRangeSource() override { close(fd_); }

    int64_t read(int64_t pos, uint8_t* out, int64_t size) override {
        int64_t end = length_;
        if (download_) {
            end = download_->waitForData(pos, abort_);
            if (end < 0) return -1;
        }
        const int64_t remaining = end - pos;
        if (remaining <= 0 || size <= 0) return 0;

        const size_t toRead = static_cast<size_t>(std::min(size, remaining));
        ssize_t n;
        do {
            n = pread(fd_, out, toRead, offset_ + pos);
        } while (n < 0 && errno == EINTR);
        return n < 0 ? -1 : n;
    }

    int64_t size() const override { return download_ ? download_->size() : length_; }

private:
    const int fd_;
    const int64_t offset_;
    const int64_t length_;
    const std::shared_ptr<GrowingFile> download_;
    const std::atomic<bool>* const abort_;
};

class MemorySource : public ByteSource {
public:
    MemorySource(const uint8_t* data, size_t size) : bytes_(data, data + size) {}

    int64_t read(int64_t pos, uint8_t* out, int64_t size) override {
        const int64_t remaining = static_cast<int64_t>(bytes_.size()) - pos;
        if (pos < 0 || remaining <= 0 || size <= 0) return 0;
        const int64_t n = std::min(size, remaining);
        std::memcpy(out, bytes_.data() + pos, static_cast<size_t>(n));
        return n;
    }

    int64_t size() const override { return static_cast<int64_t>(bytes_.size()); }

private:
    const std::vector<uint8_t> bytes_;
};

int openReadOnly(const std::string& path) {
    int fd;
    do {
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

} // namespace

std::unique_ptr<ByteSource> openFileRange(int fd, int64_t offset, int64_t length) {
    if (fd < 0 || offset < 0) return nullptr;

    if (length < 0) {
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < offset) return nullptr;
        length = st.st_size - offset;
    }

    int ownFd = dup(fd);
    if (ownFd < 0) return nullptr;
    return std::make_unique<FileRangeSource>(ownFd, offset, length, nullptr, nullptr);
}

std::unique_ptr<ByteSource> openFile(const std::string& path) {
    int fd = openReadOnly(path);
    if (fd < 0) return nullptr;
    std::unique_ptr<ByteSource> source = openFileRange(fd, 0, -1);
    close(fd);
    return source;
}

std::unique_ptr<ByteSource> openDownload(const std::string& path, const std::atomic<bool>* abort) {
    std::shared_ptr<GrowingFile> download = GrowingFile::find(path);
    if (!download) return openFile(path);
    int fd = dup(download->fd());
    if (fd < 0) return nullptr;
    return std::make_unique<FileRangeSource>(fd, 0, -1, std::move(download), abort);
}

std::unique_ptr<ByteSource> openMemory(const uint8_t* data, size_t size) {
    return std::make_unique<MemorySource>(data, size);
}

} // namespace webm
