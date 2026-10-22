import AlphaVideo
import AlphaVideoTestSupport
import RangeSource
import XCTest

/** Serves the test card's files from disk, whatever the URL's path names. */
final class FixtureSource: NSObject, RangeDataSource {
    private let lock = NSLock()
    private var count = 0

    var reads: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }

    func read(
        url: URL, offset: Int64, length: Int64, headers: [String: String],
        response: @escaping (RangeDataInfo) -> Void, data: @escaping (Data) -> Void,
        completion: @escaping (Error?) -> Void
    ) -> RangeCancellable {
        lock.lock()
        count += 1
        lock.unlock()
        DispatchQueue.global().async {
            guard let bytes = FileManager.default.contents(atPath: fixture(url.lastPathComponent)) else {
                completion(NSError(domain: "test", code: 1, userInfo: [NSLocalizedDescriptionKey: "no \(url)"]))
                return
            }
            response(RangeDataInfo(size: Int64(bytes.count), etag: nil, lastModified: nil))
            let start = Int(min(offset, Int64(bytes.count)))
            let end = Int(min(offset + length, Int64(bytes.count)))
            data(bytes.subdata(in: start..<end))
            completion(nil)
        }
        return Cancel()
    }

    final class Cancel: NSObject, RangeCancellable {
        func cancel() {}
    }
}

// Links the RangeSource product, which the player never names: it has to find the add-on by
// itself, and read a data source registered with it.
final class SourceTests: XCTestCase {
    func testPlaysThroughADataSource() throws {
        let source = FixtureSource()
        RangeDataSources.register(source, name: "fixtures")
        defer { RangeDataSources.unregister(name: "fixtures") }
        let outcome = try openInView("fixtures://card/dual_vp9.webm", dataSource: "fixtures")
        XCTAssertEqual(outcome.loaded, true, outcome.error ?? "")
        XCTAssertGreaterThan(source.reads, 0)
    }

    func testUnregisteredDataSourceFails() throws {
        let outcome = try openInView("fixtures://card/dual_vp9.webm", dataSource: "nobody")
        XCTAssertNil(outcome.code)
        XCTAssertTrue(outcome.error?.contains("No data source is registered as \"nobody\"") == true, outcome.error ?? "")
    }
}
