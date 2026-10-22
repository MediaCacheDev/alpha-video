import AlphaVideo
import Foundation

/** Shows `source` in an AlphaVideoView, through `dataSource` if given, and waits for its load or its error. */
public func openInView(_ source: String, dataSource: String? = nil, timeout: TimeInterval = 10) throws -> LoadOutcome {
    let recorder = ViewRecorder()
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

private final class ViewRecorder: NSObject, AlphaVideoViewDelegate {
    var outcome = LoadOutcome()
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
