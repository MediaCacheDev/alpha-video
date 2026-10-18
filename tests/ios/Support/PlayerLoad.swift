import AlphaVideo
import Foundation

public struct LoadOutcome {
    public var loaded = false
    public var error: String?
    public var code: String?
}

public struct LoadTimeout: Error {}

/** A 256x256 test card in tests/fixtures; the Simulator reads the Mac's files. */
public func fixture(_ name: String) -> String {
    URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        .appendingPathComponent("fixtures/card_256x256/\(name)").path
}

/** Opens `path` in a player and waits for its load or its error, on the main thread. */
public func open(_ path: String, timeout: TimeInterval = 10) throws -> LoadOutcome {
    let recorder = Recorder()
    let player = AlphaVideoPlayer()
    player.delegate = recorder
    _ = player.load(fromFile: path)
    // The player's events arrive on the main run loop
    let deadline = Date(timeIntervalSinceNow: timeout)
    while !recorder.done && Date() < deadline {
        RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.05))
    }
    player.unload()
    guard recorder.done else { throw LoadTimeout() }
    return recorder.outcome
}

private final class Recorder: NSObject, AlphaVideoPlayerDelegate {
    var outcome = LoadOutcome()
    var done = false

    func playerDidLoad(_ totalFrames: Int32, width: Int32, height: Int32) {
        outcome.loaded = true
        done = true
    }

    func playerDidEncounterError(_ error: String, code: String?) {
        outcome.error = error
        outcome.code = code
        done = true
    }
}
