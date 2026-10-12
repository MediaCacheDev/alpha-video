#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace webm {

/**
 * A cache file that a platform downloader (HttpURLConnection, NSURLSession) is still writing.
 *
 * The downloader writes into a temporary file and reports each chunk here; readers wait for bytes
 * that haven't arrived yet instead of hitting end of file, so playback can start while the rest
 * downloads. Downloads are registered under the path the finished file will have, which is the
 * path readers ask for (see openDownloadIO).
 *
 * The downloader calls begin(), setWritten() after each write, and finally finish() (after
 * renaming the temporary file into place) or fail().
 */
class GrowingFile {
public:
    /**
     * Registers a download into `partPath` that will become `path`. `expectedSize` is the
     * Content-Length, or -1 if the server didn't send one. Returns nullptr if `partPath` can't be
     * opened for reading.
     */
    static std::shared_ptr<GrowingFile> begin(const std::string& path, const std::string& partPath,
                                              int64_t expectedSize);

    /** The download writing `path`, or nullptr if none is in progress. */
    static std::shared_ptr<GrowingFile> find(const std::string& path);

    ~GrowingFile();
    GrowingFile(const GrowingFile&) = delete;
    GrowingFile& operator=(const GrowingFile&) = delete;

    /** Bytes [0, bytes) are on disk. */
    void setWritten(int64_t bytes);
    /** Every byte is on disk and the file is at its final path. */
    void finish();
    /** The download stopped; readers get an error past the bytes already written. */
    void fail();

    /**
     * Waits until there is data at `pos` or the download has ended, and returns how many bytes
     * are on disk (equal to or below `pos` means end of file). Returns -1 if the download failed
     * before reaching `pos`, or if `abort` became true while waiting.
     */
    int64_t waitForData(int64_t pos, const std::atomic<bool>* abort);

    /** The final size: the expected size, or once finished the size written. -1 if not known yet. */
    int64_t size() const;

    /** Read-only descriptor of the file, owned by this object; duplicate it to keep it. */
    int fd() const { return fd_; }

private:
    enum class State { Downloading, Finished, Failed };

    GrowingFile(std::string path, int fd, int64_t expectedSize);
    void end(State state);

    const std::string path_;
    const int fd_;
    const int64_t expectedSize_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    int64_t written_ = 0;
    State state_ = State::Downloading;
};

} // namespace webm
