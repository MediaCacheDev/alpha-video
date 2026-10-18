import AlphaVideo
import AlphaVideoTestSupport
import XCTest

// Links the AlphaVideoVPX product alone, and never names its class: the core has to find the
// module by itself, and a file in the other codec has to report NO_DECODER.
final class VPXOnlyTests: XCTestCase {
    func testRegistersLibvpxOnly() {
        let decoders = AlphaVideoCodecs.softwareDecoders
        XCTAssertEqual(decoders.count, 1, "\(decoders)")
        XCTAssertTrue(decoders.first?.hasPrefix("libvpx ") == true, "\(decoders)")
    }

    func testPlaysVP9() throws {
        let outcome = try open(fixture("dual_vp9.webm"))
        XCTAssertEqual(outcome.loaded, true, outcome.error ?? "")
    }

    func testAV1ReportsNoDecoder() throws {
        let outcome = try open(fixture("dual_av1.webm"))
        XCTAssertEqual(outcome.code, AlphaVideoErrorNoDecoder)
        XCTAssertTrue(outcome.error?.contains("(the av1 codec module)") == true, outcome.error ?? "")
    }
}
