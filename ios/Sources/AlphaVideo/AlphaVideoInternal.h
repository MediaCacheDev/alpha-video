/**
 * AlphaVideoInternal.h
 * Shared by the library's own files; not part of the public API.
 */

#import "AlphaVideo.h"

#ifdef __cplusplus
#include <memory>
#include "RangeSource.h"
#endif

NS_ASSUME_NONNULL_BEGIN

#ifdef __cplusplus
/** The range-source add-on's table, or NULL without it. */
const RangeSourcePlugin *_Nullable AlphaVideoRangeSourcePlugin(void);
#endif

/**
 * A native reader over a URL or a data source, fetching on its own thread. With the range-source
 * add-on, it reads by range, through blocks in memory and an optional store. Without it, the
 * basic input: a URL downloaded whole into a temporary file, read as it arrives. Closes once the
 * last holder (the view, a player) lets go.
 */
@interface AlphaVideoSourceReader : NSObject

/**
 * Opens the source at 0 to learn its size and version, unless the store holds it whole. Blocks
 * on the network, so not on the main queue.
 *
 * @param dataSource nil for the built-in URLSession fetcher (http and https only); with the add-on
 * @param storage "disk", "memory" or "none"; with the add-on
 * @param key what the store calls the file; with the add-on
 */
+ (nullable instancetype)openURL:(NSURL *)url
                      dataSource:(nullable id<AlphaVideoDataSource>)dataSource
                         headers:(NSDictionary<NSString *, NSString *> *)headers
                         storage:(NSString *)storage
                             key:(NSString *)key
                 maxStorageBytes:(int64_t)maxStorageBytes
                           error:(NSString *_Nullable *_Nullable)error;

/** Reads the whole file into the store. Blocks; NO with `error` set if it fails. */
- (BOOL)fetchAll:(NSString *_Nullable *_Nullable)error;

#ifdef __cplusplus
@property (nonatomic, readonly) std::shared_ptr<webm::SourceReader> rangeReader;
#endif

@end

@interface AlphaVideoPlayer (SourceReader)
- (BOOL)loadFromReader:(AlphaVideoSourceReader *)reader;
@end

@interface AlphaVideoMetalView (SourceReader)
- (BOOL)loadFromReader:(AlphaVideoSourceReader *)reader;
@end

@interface AlphaVideoCodecs ()
/** Registers the linked codec modules, once. Before a player opens a file. */
+ (void)discover;
@end

/** The file an asset://, file:// or absolute-path source names; nil for anything else. */
NSString *_Nullable AlphaVideoLocalPathForSource(NSString *uri);

NS_ASSUME_NONNULL_END
