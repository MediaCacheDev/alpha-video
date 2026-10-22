import AlphaVideo
import XCTest

final class WithoutRangeSourceTests: XCTestCase {
    func testTheAddOnIsLeftOut() {
        XCTAssertNil(NSClassFromString("RangeSource"))
    }

    func testDataSourceReportsNoSourceModule() throws {
        let outcome = try openInView("fixtures://card/dual_vp9.webm", dataSource: "fixtures")
        XCTAssertEqual(outcome.code, AlphaVideoErrorNoSourceModule)
        XCTAssertTrue(outcome.error?.contains("needs the range-source add-on") == true, outcome.error ?? "")
    }

    func testPlaysAFile() throws {
        let outcome = try openInView(fixture("dual_vp9.webm"))
        XCTAssertEqual(outcome.loaded, true, outcome.error ?? "")
    }
}

struct Outcome {
    var loaded = false
    var error: String?
    var code: String?
}

struct LoadTimeout: Error {}

/** A 256x256 test card in tests/fixtures; the Simulator reads the Mac's files. */
func fixture(_ name: String) -> String {
    URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        .appendingPathComponent("fixtures/card_256x256/\(name)").path
}

/** Shows `source` in an AlphaVideoView and waits for its load or its error. */
func openInView(_ source: String, dataSource: String? = nil, timeout: TimeInterval = 10) throws -> Outcome {
    let recorder = Recorder()
    let view = AlphaVideoView(frame: CGRect(x: 0, y: 0, width: 256, height: 256))
    view.delegate = recorder
    view.dataSource = dataSource
    view.source = source
    let deadline = Date(timeIntervalSinceNow: timeout)
    while !recorder.done && Date() < deadline {
        RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
    }
    view.source = nil
    guard recorder.done else { throw LoadTimeout() }
    return recorder.outcome
}

private final class Recorder: NSObject, AlphaVideoViewDelegate {
    var outcome = Outcome()
    var done = false

    func alphaVideoView(_ view: AlphaVideoView, didLoadFrames totalFrames: Int32, width: Int32, height: Int32) {
        outcome.loaded = true
        done = true
    }

    func alphaVideoView(_ view: AlphaVideoView, didFailWithError error: String, code: String?) {
        outcome.error = error
        outcome.code = code
        done = true
    }
}
