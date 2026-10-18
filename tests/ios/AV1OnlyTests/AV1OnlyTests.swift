import AlphaVideo
import AlphaVideoTestSupport
import XCTest

// Links the AlphaVideoAV1 product alone, and never names its class: the core has to find the
// module by itself, and a file in the other codec has to report NO_DECODER.
final class AV1OnlyTests: XCTestCase {
    func testRegistersDav1dOnly() {
        let decoders = AlphaVideoCodecs.softwareDecoders
        XCTAssertEqual(decoders.count, 1, "\(decoders)")
        XCTAssertTrue(decoders.first?.hasPrefix("dav1d ") == true, "\(decoders)")
    }

    func testPlaysAV1() throws {
        let outcome = try open(fixture("dual_av1_10bit.webm"))
        XCTAssertEqual(outcome.loaded, true, outcome.error ?? "")
    }

    func testVP9ReportsNoDecoder() throws {
        let outcome = try open(fixture("dual_vp9.webm"))
        XCTAssertEqual(outcome.code, AlphaVideoErrorNoDecoder)
        XCTAssertTrue(outcome.error?.contains("(the vpx codec module)") == true, outcome.error ?? "")
    }
}
