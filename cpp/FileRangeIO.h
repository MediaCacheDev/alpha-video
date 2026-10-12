#pragma once

#include <atomic>
#include <cstdint>
#include <string>

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

/**
 * FFmpeg custom I/O over a downloaded cache file, which may still be downloading (GrowingFile).
 *
 * While it downloads, reads wait for bytes that haven't arrived. They fail with AVERROR(EIO) if
 * the download fails first, and with AVERROR_EXIT once `abort` (may be null) becomes true, so a
 * player can unload without waiting for the network. A finished file is read like any other.
 *
 * Returns nullptr if the file can't be opened. Free with closeFileRangeIO.
 */
AVIOContext* openDownloadIO(const std::string& path, const std::atomic<bool>* abort);

/** Frees the context, its buffer and the duplicated descriptor. Call after avformat_close_input. */
void closeFileRangeIO(AVIOContext** ctx);

} // namespace webm
