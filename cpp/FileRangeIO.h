#pragma once

#include <cstdint>

struct AVIOContext;

namespace webm {

/**
 * FFmpeg custom I/O over bytes [offset, offset + length) of a file descriptor.
 *
 * This is how a video stored inside an APK (AssetManager.openFd) or behind a content:// URI is
 * demuxed in place, without first copying it to a file. The descriptor is duplicated, so the
 * caller still owns and closes the one it passed. A negative length means "to end of file".
 *
 * Returns nullptr if the range can't be opened.
 */
AVIOContext* openFileRangeIO(int fd, int64_t offset, int64_t length);

/** Frees the context, its buffer and the duplicated descriptor. Call after avformat_close_input. */
void closeFileRangeIO(AVIOContext** ctx);

} // namespace webm
