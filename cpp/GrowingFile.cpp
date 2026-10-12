#include "GrowingFile.h"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_map>

namespace webm {

namespace {

// How often a waiting reader checks its abort flag; nothing signals the condition when it changes.
constexpr auto kAbortPollInterval = std::chrono::milliseconds(50);

std::mutex registryMutex;
std::unordered_map<std::string, std::shared_ptr<GrowingFile>> registry;

} // namespace

std::shared_ptr<GrowingFile> GrowingFile::begin(const std::string& path, const std::string& partPath,
                                                int64_t expectedSize) {
    int fd;
    do {
        fd = open(partPath.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return nullptr;

    std::shared_ptr<GrowingFile> file(new GrowingFile(path, fd, expectedSize));
    std::lock_guard<std::mutex> lock(registryMutex);
    registry[path] = file;
    return file;
}

std::shared_ptr<GrowingFile> GrowingFile::find(const std::string& path) {
    std::lock_guard<std::mutex> lock(registryMutex);
    auto it = registry.find(path);
    return it == registry.end() ? nullptr : it->second;
}

GrowingFile::GrowingFile(std::string path, int fd, int64_t expectedSize)
    : path_(std::move(path)), fd_(fd), expectedSize_(expectedSize) {}

GrowingFile::~GrowingFile() {
    close(fd_);
}

void GrowingFile::setWritten(int64_t bytes) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        written_ = bytes;
    }
    changed_.notify_all();
}

void GrowingFile::finish() {
    end(State::Finished);
}

void GrowingFile::fail() {
    end(State::Failed);
}

void GrowingFile::end(State state) {
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto it = registry.find(path_);
        if (it != registry.end() && it->second.get() == this) registry.erase(it);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }
    changed_.notify_all();
}

int64_t GrowingFile::waitForData(int64_t pos, const std::atomic<bool>* abort) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (written_ <= pos && state_ == State::Downloading) {
        if (abort && abort->load()) return -1;
        changed_.wait_for(lock, kAbortPollInterval);
    }
    if (written_ <= pos && state_ == State::Failed) return -1;
    return written_;
}

int64_t GrowingFile::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Finished) return written_;
    return expectedSize_;
}

} // namespace webm
