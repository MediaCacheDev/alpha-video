#pragma once

#include "RangeSourceClient.h"

#include <cstdint>
#include <memory>
#include <string>

namespace webm {

/**
 * The bytes of a video, read at any position: a file, a range of one, a URL or an app's data
 * source read through a SourceReader, or memory. The demuxer reads through one of these.
 */
class ByteSource {
public:
    /** What read() returns when the bytes at `pos` aren't there yet and it won't wait for them. */
    static constexpr int64_t kPending = -2;

    virtual ~ByteSource() = default;

    /**
     * Reads up to `size` bytes at `pos` into `out` and returns how many it read: fewer near the
     * end, 0 at or past the end, -1 on an error. A range reader returns once some bytes at `pos`
     * have arrived. A source that can't wait (the web's, which fetches asynchronously) returns
     * kPending instead; the demuxer then stops, and carries on when called again.
     */
    virtual int64_t read(int64_t pos, uint8_t* out, int64_t size) = 0;

    /** The total size, or -1 while it isn't known (a response without a Content-Length). */
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
 * A URL or an app's data source, through a SourceReader: the range-source add-on's reader, or the
 * basic input's download.
 *
 * Reads wait for bytes being fetched. They fail if the fetch fails, or once `abort` (may be null;
 * must outlive the source) is raised, so a player can unload without waiting for the network.
 * The reader may be shared, by a probe and then the player.
 */
std::unique_ptr<ByteSource> openReader(std::shared_ptr<SourceReader> reader, const AbortFlag* abort);

/** A copy of `size` bytes at `data`. */
std::unique_ptr<ByteSource> openMemory(const uint8_t* data, size_t size);

} // namespace webm
