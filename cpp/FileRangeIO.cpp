#include "FileRangeIO.h"

#include <algorithm>
#include <cerrno>
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
};

int readRange(void* opaque, uint8_t* buf, int bufSize) {
    auto* range = static_cast<FileRange*>(opaque);
    int64_t remaining = range->length - range->pos;
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
    if (whence & AVSEEK_SIZE) return range->length;

    int64_t target;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = range->pos + offset; break;
        case SEEK_END: target = range->length + offset; break;
        default: return AVERROR(EINVAL);
    }
    if (target < 0 || target > range->length) return AVERROR(EINVAL);
    range->pos = target;
    return target;
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

    auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
    if (!buffer) {
        close(ownFd);
        return nullptr;
    }

    auto* range = new FileRange{ownFd, offset, length, 0};
    AVIOContext* ctx = avio_alloc_context(buffer, kBufferSize, 0, range, readRange, nullptr, seekRange);
    if (!ctx) {
        av_free(buffer);
        close(ownFd);
        delete range;
        return nullptr;
    }
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
