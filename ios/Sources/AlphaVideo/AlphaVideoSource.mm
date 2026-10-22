/**
 * AlphaVideoSource.mm
 * Reading URLs and data sources: the built-in URLSession fetcher, and the reader it feeds, the
 * range-source add-on's through its C table, or without the add-on the basic input's download.
 * A data source is read by the add-on, which the player finds at runtime.
 */

#import "AlphaVideoInternal.h"


#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace {

/**
 * Bytes handed from callbacks on any thread to the reader's fetch thread: a response, then data,
 * then the end or an error. With flow control, the producer is paused while more than kHighBytes
 * wait and resumed under kLowBytes.
 */
class Pipe {
public:
    static constexpr size_t kHighBytes = 1024 * 1024;
    static constexpr size_t kLowBytes = 256 * 1024;

    /** Called under the pipe's lock with true to pause the producer, false to resume it. */
    void setFlowControl(std::function<void(bool)> pause) {
        std::lock_guard<std::mutex> lock(mutex_);
        pause_ = std::move(pause);
    }

    /** `skip`: bytes of the data to drop first, for a server that answered from 0. */
    void respond(const webm::SourceInfo& info, int64_t skip = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (responded_ || done_) return;
        responded_ = true;
        info_ = info;
        skip_ = skip;
        changed_.notify_all();
    }

    void push(const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_ || !responded_) return;
        if (skip_ > 0) {
            size_t dropped = static_cast<size_t>(std::min<int64_t>(skip_, static_cast<int64_t>(size)));
            skip_ -= static_cast<int64_t>(dropped);
            data += dropped;
            size -= dropped;
        }
        if (size == 0) return;
        buffer_.append(reinterpret_cast<const char*>(data), size);
        if (pause_ && !paused_ && bufferedLocked() > kHighBytes) {
            paused_ = true;
            pause_(true);
        }
        changed_.notify_all();
    }

    /** An empty error is the end of the data. Only the first call counts. */
    void finish(const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_) return;
        done_ = true;
        error_ = error;
        changed_.notify_all();
    }

    void cancel() { finish("Cancelled"); }

    bool waitResponse(webm::SourceInfo& info, std::string& error) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return responded_ || done_; });
        if (!responded_) {
            error = error_.empty() ? "The source ended without a response" : error_;
            return false;
        }
        info = info_;
        return true;
    }

    /** Bytes read, 0 at the end, or -1 with `error` set; the data before an error comes first. */
    int64_t read(uint8_t* out, int64_t size, std::string& error) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return bufferedLocked() > 0 || done_; });
        size_t available = bufferedLocked();
        if (available > 0) {
            size_t n = std::min(available, static_cast<size_t>(size));
            memcpy(out, buffer_.data() + offset_, n);
            offset_ += n;
            if (offset_ == buffer_.size()) {
                buffer_.clear();
                offset_ = 0;
            } else if (offset_ > kHighBytes) {
                buffer_.erase(0, offset_);
                offset_ = 0;
            }
            if (paused_ && bufferedLocked() < kLowBytes) {
                paused_ = false;
                pause_(false);
            }
            return static_cast<int64_t>(n);
        }
        if (!error_.empty()) {
            error = error_;
            return -1;
        }
        return 0;
    }

private:
    size_t bufferedLocked() const { return buffer_.size() - offset_; }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::function<void(bool)> pause_;
    bool paused_ = false;
    bool responded_ = false;
    bool done_ = false;
    webm::SourceInfo info_;
    int64_t skip_ = 0;
    std::string buffer_;
    size_t offset_ = 0;
    std::string error_;
};

std::string stdString(NSString* text) {
    return text ? std::string(text.UTF8String) : std::string();
}

NSString* nsString(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

std::string describe(NSError* error) {
    return stdString(error.localizedDescription ?: @"The read failed");
}

} // namespace

#pragma mark - The built-in URLSession fetcher

/** One task's pipe, and where its read starts. */
@interface AlphaVideoTaskState : NSObject {
@public
    std::shared_ptr<Pipe> pipe;
    int64_t position;
}
@end

@implementation AlphaVideoTaskState
@end

/** Routes a session's task callbacks to each task's pipe. Callbacks run on the session's queue. */
@interface AlphaVideoSessionDelegate : NSObject <NSURLSessionDataDelegate>
- (void)track:(NSURLSessionTask *)task state:(AlphaVideoTaskState *)state;
@end

@implementation AlphaVideoSessionDelegate {
    // @synchronized (self)
    NSMutableDictionary<NSNumber *, AlphaVideoTaskState *> *_tasks;
}

- (instancetype)init {
    if ((self = [super init])) _tasks = [NSMutableDictionary dictionary];
    return self;
}

- (void)track:(NSURLSessionTask *)task state:(AlphaVideoTaskState *)state {
    @synchronized (self) {
        _tasks[@(task.taskIdentifier)] = state;
    }
}

- (nullable AlphaVideoTaskState *)stateOf:(NSURLSessionTask *)task remove:(BOOL)remove {
    @synchronized (self) {
        AlphaVideoTaskState *state = _tasks[@(task.taskIdentifier)];
        if (remove) [_tasks removeObjectForKey:@(task.taskIdentifier)];
        return state;
    }
}

static int64_t AlphaVideoTotalFromContentRange(NSString *contentRange, int64_t *start) {
    // "bytes 100-199/1000", "bytes 100-199/*" or, answering 416, "bytes */1000"
    NSScanner *scanner = [NSScanner scannerWithString:contentRange ?: @""];
    long long first = -1, total = -1;
    if (![scanner scanString:@"bytes" intoString:nil]) return -1;
    if (![scanner scanString:@"*" intoString:nil]) {
        if (![scanner scanLongLong:&first]) return -1;
        [scanner scanUpToString:@"/" intoString:nil];
    }
    if (start) *start = first;
    if (![scanner scanString:@"/" intoString:nil] || ![scanner scanLongLong:&total]) return -1;
    return total;
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
didReceiveResponse:(NSURLResponse *)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    AlphaVideoTaskState *state = [self stateOf:dataTask remove:NO];
    if (!state) {
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    NSHTTPURLResponse *http = [response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse *)response : nil;
    webm::SourceInfo info;
    info.etag = stdString([http valueForHTTPHeaderField:@"ETag"]);
    info.lastModified = stdString([http valueForHTTPHeaderField:@"Last-Modified"]);
    NSString *encoding = [http valueForHTTPHeaderField:@"Content-Encoding"];
    if (encoding.length > 0 && [encoding caseInsensitiveCompare:@"identity"] != NSOrderedSame) {
        // URLSession decodes it, and the offsets would be into the decoded bytes
        state->pipe->finish(stdString([NSString stringWithFormat:@"%@ came compressed (%@)", response.URL, encoding]));
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    NSInteger status = http ? http.statusCode : 200;
    NSString *contentRange = [http valueForHTTPHeaderField:@"Content-Range"];
    if (status == 206) {
        int64_t start = -1;
        info.size = AlphaVideoTotalFromContentRange(contentRange, &start);
        if (start != state->position) {
            state->pipe->finish(stdString([NSString stringWithFormat:@"Asked %@ for bytes from %lld and got %@",
                                           response.URL, (long long)state->position, contentRange]));
            completionHandler(NSURLSessionResponseCancel);
            return;
        }
        state->pipe->respond(info);
    } else if (status == 200) {
        // No ranges: the whole file, read past the bytes before the position
        info.size = response.expectedContentLength >= 0 ? response.expectedContentLength : -1;
        state->pipe->respond(info, state->position);
    } else if (status == 416) {
        // At or past the end: a read that ends at once
        info.size = AlphaVideoTotalFromContentRange(contentRange, nullptr);
        state->pipe->respond(info);
        state->pipe->finish("");
        completionHandler(NSURLSessionResponseCancel);
        return;
    } else {
        state->pipe->finish(stdString([NSString stringWithFormat:@"HTTP %ld for %@", (long)status, response.URL]));
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)dataTask didReceiveData:(NSData *)data {
    AlphaVideoTaskState *state = [self stateOf:dataTask remove:NO];
    if (!state) return;
    [data enumerateByteRangesUsingBlock:^(const void *bytes, NSRange range, BOOL *stop) {
        state->pipe->push(static_cast<const uint8_t *>(bytes), range.length);
    }];
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    AlphaVideoTaskState *state = [self stateOf:task remove:YES];
    if (!state) return;
    state->pipe->finish(error ? describe(error) : "");
}

@end

namespace {

/**
 * Reads a URL with URLSession: a `Range: bytes=N-` request per open, kept open as the reader
 * reads on, and suspended while the reader has a megabyte waiting.
 */
class URLSessionUpstream : public webm::Upstream {
public:
    URLSessionUpstream(NSURL* url, NSDictionary<NSString*, NSString*>* headers)
        : url_(url), headers_([headers copy]), delegate_([AlphaVideoSessionDelegate new]) {
        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration defaultSessionConfiguration];
        // The reader's store is the cache
        configuration.URLCache = nil;
        configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        configuration.timeoutIntervalForRequest = 30;
        session_ = [NSURLSession sessionWithConfiguration:configuration delegate:delegate_ delegateQueue:nil];
    }

    ~URLSessionUpstream() override {
        close();
        // The session holds its delegate until invalidated
        [session_ invalidateAndCancel];
    }

    bool open(int64_t position, webm::SourceInfo& info, std::string& error) override {
        close();
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url_];
        [headers_ enumerateKeysAndObjectsUsingBlock:^(NSString* name, NSString* value, BOOL* stop) {
            [request setValue:value forHTTPHeaderField:name];
        }];
        // Offsets are into the file as stored, so no transparent gzip
        [request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
        [request setValue:[NSString stringWithFormat:@"bytes=%lld-", (long long)position] forHTTPHeaderField:@"Range"];
        NSURLSessionDataTask* task = [session_ dataTaskWithRequest:request];

        auto pipe = std::make_shared<Pipe>();
        pipe->setFlowControl([task](bool pause) {
            if (pause) [task suspend];
            else [task resume];
        });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_) {
                error = "Cancelled";
                return false;
            }
            task_ = task;
            pipe_ = pipe;
        }
        AlphaVideoTaskState* state = [AlphaVideoTaskState new];
        state->pipe = pipe;
        state->position = position;
        [delegate_ track:task state:state];
        [task resume];
        return pipe->waitResponse(info, error);
    }

    int64_t read(uint8_t* out, int64_t size, std::string& error) override {
        std::shared_ptr<Pipe> pipe = currentPipe();
        if (!pipe) {
            error = "Not open";
            return -1;
        }
        return pipe->read(out, size, error);
    }

    void close() override {
        NSURLSessionDataTask* task;
        std::shared_ptr<Pipe> pipe;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task = task_;
            pipe = std::move(pipe_);
            task_ = nil;
        }
        if (pipe) pipe->cancel();
        [task cancel];
    }

    void cancel() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
        }
        close();
    }

private:
    std::shared_ptr<Pipe> currentPipe() {
        std::lock_guard<std::mutex> lock(mutex_);
        return pipe_;
    }

    NSURL* const url_;
    NSDictionary<NSString*, NSString*>* const headers_;
    AlphaVideoSessionDelegate* const delegate_;
    NSURLSession* session_;
    std::mutex mutex_;
    NSURLSessionDataTask* task_ = nil;
    std::shared_ptr<Pipe> pipe_;
    bool cancelled_ = false;
};

} // namespace

#pragma mark - AlphaVideoSourceReader

NSString *const AlphaVideoErrorNoSourceModule = @"NO_SOURCE_MODULE";

/**
 * The add-on's class, as the player uses it (range-source's RangeSource.h). The player doesn't
 * link the add-on, so it finds the class by name.
 */
@protocol AlphaVideoRangeSourceAddOn
@property (class, nonatomic, readonly) const void *plugin;
+ (BOOL)getUpstream:(void *)upstream
    forDataSourceNamed:(NSString *)name
                   URL:(NSURL *)url
               headers:(NSDictionary<NSString *, NSString *> *)headers;
@end

static Class<AlphaVideoRangeSourceAddOn> AlphaVideoRangeSourceClass(void) {
    Class addOn = NSClassFromString(@"RangeSource");
    return [addOn respondsToSelector:@selector(plugin)]
            && [addOn respondsToSelector:@selector(getUpstream:forDataSourceNamed:URL:headers:)]
        ? addOn : Nil;
}

const RangeSourcePlugin *AlphaVideoRangeSourcePlugin(void) {
    static const RangeSourcePlugin *plugin = [] {
        Class<AlphaVideoRangeSourceAddOn> addOn = AlphaVideoRangeSourceClass();
        if (!addOn) return static_cast<const RangeSourcePlugin *>(nullptr);
        std::string why;
        const RangeSourcePlugin *usable = webm::usablePlugin(static_cast<const RangeSourcePlugin *>([addOn plugin]), why);
        if (!usable) NSLog(@"[AlphaVideo] Not using range-source: %s", why.c_str());
        return usable;
    }();
    return plugin;
}

// Caches/alpha-video on disk, or the memory store's name
static NSString *const AlphaVideoStoreName = @"alpha-video";

static std::shared_ptr<webm_rs::Store> AlphaVideoStore(const RangeSourcePlugin *plugin, NSString *storage,
                                                       int64_t maxStorageBytes) {
    if ([storage isEqualToString:@"disk"]) {
        NSString *caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
        return webm_rs::Store::disk(plugin, stdString([caches stringByAppendingPathComponent:AlphaVideoStoreName]),
                                    maxStorageBytes);
    }
    if ([storage isEqualToString:@"memory"]) return webm_rs::Store::memory(plugin, stdString(AlphaVideoStoreName), maxStorageBytes);
    return nullptr;
}

@implementation AlphaVideoSourceReader {
    std::shared_ptr<webm::SourceReader> _reader;
}

+ (instancetype)openURL:(NSURL *)url
             dataSource:(NSString *)dataSource
                headers:(NSDictionary<NSString *, NSString *> *)headers
                storage:(NSString *)storage
                    key:(NSString *)key
        maxStorageBytes:(int64_t)maxStorageBytes
                  error:(NSString **)error {
    const RangeSourcePlugin *plugin = AlphaVideoRangeSourcePlugin();
    std::string openError;
    std::shared_ptr<webm::SourceReader> reader;
    if (dataSource) {
        RangeSourceUpstream upstream{};
        if (!plugin || ![AlphaVideoRangeSourceClass() getUpstream:&upstream forDataSourceNamed:dataSource URL:url
                                                          headers:headers]) {
            if (error) *error = [NSString stringWithFormat:@"No data source is registered as \"%@\" (RangeDataSources)", dataSource];
            return nil;
        }
        reader = webm_rs::RangeReader::open(plugin, upstream, AlphaVideoStore(plugin, storage, maxStorageBytes),
                                            stdString(key), webm_rs::ReaderOptions(), openError);
    } else if ([url.scheme isEqualToString:@"https"] || [url.scheme isEqualToString:@"http"]) {
        auto upstream = std::make_unique<URLSessionUpstream>(url, headers);
        reader = plugin
            ? std::shared_ptr<webm::SourceReader>(webm_rs::RangeReader::open(
                  plugin, std::move(upstream), AlphaVideoStore(plugin, storage, maxStorageBytes), stdString(key),
                  webm_rs::ReaderOptions(), openError))
            : webm_rs::DownloadReader::open(std::move(upstream), stdString(NSTemporaryDirectory()), openError);
    } else {
        if (error) *error = [NSString stringWithFormat:@"Not an http(s) URL: %@", url];
        return nil;
    }
    if (!reader) {
        if (error) *error = nsString(openError);
        return nil;
    }
    AlphaVideoSourceReader *result = [AlphaVideoSourceReader new];
    result->_reader = std::move(reader);
    return result;
}

- (BOOL)fetchAll:(NSString **)error {
    if (_reader->fetchAll()) return YES;
    if (error) {
        std::string message = _reader->lastError();
        *error = message.empty() ? @"Reading the video failed" : nsString(message);
    }
    return NO;
}

- (std::shared_ptr<webm::SourceReader>)rangeReader {
    return _reader;
}

@end
