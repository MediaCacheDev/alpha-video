#include "FileRangeIO.h"

#include "GrowingFile.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace webm {

namespace {

constexpr int kBufferSize = 32768;

struct FileRange {
    int fd;
    int64_t offset;
    int64_t length;
    int64_t pos;
    // Set when the file is still downloading; `length` is unused then.
    std::shared_ptr<GrowingFile> download;
    const std::atomic<bool>* abort;

    int64_t size() const { return download ? download->size() : length; }
};

int readRange(void* opaque, uint8_t* buf, int bufSize) {
    auto* range = static_cast<FileRange*>(opaque);
    int64_t end = range->length;
    if (range->download) {
        end = range->download->waitForData(range->pos, range->abort);
        if (end < 0) return (range->abort && range->abort->load()) ? AVERROR_EXIT : AVERROR(EIO);
    }
    int64_t remaining = end - range->pos;
    if (remaining <= 0) return AVERROR_EOF;

    size_t toRead = static_cast<size_t>(std::min<int64_t>(bufSize, remaining));
    ssize_t n;
    do {
        n = pread(range->fd, buf, toRead, range->offset + range->pos);
    } while (n < 0 && errno == EINTR);

    if (n < 0) return AVERROR(errno);
    if (n == 0) return AVERROR_EOF;
    range->pos += n;
    return static_cast<int>(n);
}

int64_t seekRange(void* opaque, int64_t offset, int whence) {
    auto* range = static_cast<FileRange*>(opaque);
    const int64_t size = range->size();
    if (whence & AVSEEK_SIZE) return size >= 0 ? size : AVERROR(ENOSYS);

    int64_t target;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = range->pos + offset; break;
        case SEEK_END:
            if (size < 0) return AVERROR(ENOSYS);
            target = size + offset;
            break;
        default: return AVERROR(EINVAL);
    }
    if (target < 0 || (size >= 0 && target > size)) return AVERROR(EINVAL);
    range->pos = target;
    return target;
}

// Takes ownership of range->fd.
AVIOContext* openContext(FileRange* range) {
    auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
    if (!buffer) {
        close(range->fd);
        delete range;
        return nullptr;
    }

    AVIOContext* ctx = avio_alloc_context(buffer, kBufferSize, 0, range, readRange, nullptr, seekRange);
    if (!ctx) {
        av_free(buffer);
        close(range->fd);
        delete range;
        return nullptr;
    }
    return ctx;
}

} // namespace

AVIOContext* openFileRangeIO(int fd, int64_t offset, int64_t length) {
    if (fd < 0 || offset < 0) return nullptr;

    if (length < 0) {
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size < offset) return nullptr;
        length = st.st_size - offset;
    }

    int ownFd = dup(fd);
    if (ownFd < 0) return nullptr;
    return openContext(new FileRange{ownFd, offset, length, 0, nullptr, nullptr});
}

AVIOContext* openDownloadIO(const std::string& path, const std::atomic<bool>* abort) {
    std::shared_ptr<GrowingFile> download = GrowingFile::find(path);
    if (download) {
        int fd = dup(download->fd());
        if (fd < 0) return nullptr;
        return openContext(new FileRange{fd, 0, -1, 0, std::move(download), abort});
    }

    int fd;
    do {
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return nullptr;
    AVIOContext* ctx = openFileRangeIO(fd, 0, -1);
    close(fd);
    return ctx;
}

void closeFileRangeIO(AVIOContext** ctx) {
    if (!ctx || !*ctx) return;
    auto* range = static_cast<FileRange*>((*ctx)->opaque);
    av_freep(&(*ctx)->buffer);
    avio_context_free(ctx);
    if (range) {
        close(range->fd);
        delete range;
    }
}

} // namespace webm
