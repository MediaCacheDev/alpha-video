#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace webm {

/**
 * The bytes of a video, read at any position: a file, a range of one, a download still in
 * progress (GrowingFile), or memory. The demuxer reads through one of these.
 */
class ByteSource {
public:
    virtual ~ByteSource() = default;

    /**
     * Reads up to `size` bytes at `pos` into `out` and returns how many it read: fewer near the
     * end, 0 at or past the end, -1 on an error. A download returns once some bytes at `pos`
     * have arrived.
     */
    virtual int64_t read(int64_t pos, uint8_t* out, int64_t size) = 0;

    /** The total size, or -1 while it isn't known (a download without a Content-Length). */
    virtual int64_t size() const = 0;
};

/**
 * Bytes [offset, offset + length) of a file descriptor: a video stored inside an APK
 * (AssetManager.openFd) or behind a content:// URI, read in place. The descriptor is duplicated,
 * so the caller still owns and closes the one it passed. A negative length means "to end of file".
 *
 * Returns nullptr if the range can't be opened.
 */
std::unique_ptr<ByteSource> openFileRange(int fd, int64_t offset, int64_t length);

/** A whole file. Returns nullptr if it can't be opened. */
std::unique_ptr<ByteSource> openFile(const std::string& path);

/**
 * A downloaded cache file, which may still be downloading (GrowingFile).
 *
 * While it downloads, reads wait for bytes that haven't arrived. They fail if the download fails
 * first, or once `abort` (may be null; must outlive the source) becomes true, so a player can
 * unload without waiting for the network. A finished file is read like any other.
 *
 * Returns nullptr if the file can't be opened.
 */
std::unique_ptr<ByteSource> openDownload(const std::string& path, const std::atomic<bool>* abort);

/** A copy of `size` bytes at `data`. */
std::unique_ptr<ByteSource> openMemory(const uint8_t* data, size_t size);

} // namespace webm
