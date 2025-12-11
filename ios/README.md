# iOS WebM Alpha Player

A standalone iOS framework for high-performance WebM VP9 video playback with alpha transparency support.

## Features

- 🎬 **VP9 Alpha Transparency** - Full support for WebM videos with alpha channel
- ⚡ **High Performance** - Native C++ core with GPU-accelerated rendering
- 📱 **Metal Rendering** - Hardware-accelerated YUVA→RGBA conversion
- 🔄 **Zero-Copy Pipeline** - Efficient memory usage with LRU frame caching
- 🎯 **UIView Compatible** - Works with Auto Layout and proper z-ordering

## Installation

### CocoaPods

Add to your `Podfile`:

```ruby
pod 'WebMAlphaPlayer-iOS', '~> 1.0.0'
```

### Swift Package Manager

Add to your `Package.swift`:

```swift
dependencies: [
    .package(url: "https://github.com/danielkpl2/webm-alpha-player-ios.git", from: "1.0.0")
]
```

### Manual

Download the framework from releases and add to your Xcode project.

## Usage

### Storyboard

1. Add a `UIView` to your storyboard
2. Set class to `WebMAlphaMetalPlayerView`
3. Connect outlet in your view controller

### Programmatic

```swift
import WebMAlphaPlayer_iOS

class ViewController: UIViewController {
    private var playerView: WebMAlphaMetalPlayerView!

    override func viewDidLoad() {
        super.viewDidLoad()

        // Create player view
        playerView = WebMAlphaMetalPlayerView(frame: CGRect(x: 0, y: 0, width: 300, height: 300))
        view.addSubview(playerView)

        // Load and play
        if let url = Bundle.main.url(forResource: "animation", withExtension: "webm") {
            playerView.load(from: url)
            playerView.play()
        }
    }
}
```

## API Reference

### WebMAlphaMetalPlayerView

| Method | Description |
|--------|-------------|
| `load(from: URL)` | Load from file URL |
| `load(from: Data)` | Load from Data object |
| `load(from: String)` | Load from HTTP/HTTPS URL |
| `play()` | Start playback |
| `pause()` | Pause playback |
| `stop()` | Stop and reset |
| `seek(toFrame: Int)` | Seek to specific frame |
| `setPlaybackSpeed(_ speed: Double)` | Set playback speed (0.5x to 2.0x) |

### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `loops` | Bool | true | Loop playback |
| `autoPlay` | Bool | true | Auto-play when loaded |
| `playbackSpeed` | Double | 1.0 | Playback speed multiplier |

### Events

```swift
playerView.playerDelegate = self

extension ViewController: WebMAlphaPlayerDelegate {
    func playerDidLoad(totalFrames: Int, width: Int, height: Int) {
        // Video loaded successfully
    }

    func playerDidStartPlaying() {
        // Playback started
    }

    func playerDidPause() {
        // Playback paused
    }

    func playerDidFinish() {
        // Playback finished
    }

    func playerDidLoop() {
        // Playback looped
    }

    func player(didFailWithError error: Error) {
        // Error occurred
    }
}
```

## Requirements

- **iOS**: 12.0+
- **Architecture**: arm64, x86_64 (simulator)
- **Metal**: Required for GPU acceleration

## Technical Details

### Architecture

```
┌─────────────────┐    ┌─────────────────┐
│   iOS App       │    │   Obj-C++ Bridge│
│   Swift/Obj-C   │◄──►│   WebMAlphaPlayer│
│   ViewController│    │   (C++)         │
└─────────────────┘    └─────────────────┘
         │                       │
         ▼                       ▼
┌─────────────────┐    ┌─────────────────┐
│   MTKView       │    │   Metal         │
│   Rendering     │    │   YUVA→RGBA     │
│   Pipeline      │    │   GPU Shader    │
└─────────────────┘    └─────────────────┘
```

### Performance

- **Memory Usage**: ~50MB for 250-frame cache
- **CPU Usage**: <5% during playback (GPU-accelerated)
- **Startup Time**: <2 seconds for typical videos

## License

LGPL v2.1+

## Building from Source

### Prerequisites

```bash
# Install Xcode 12+
# Install CocoaPods
# Install FFmpeg build tools
```

### Build Commands

```bash
# Build FFmpeg for iOS
./scripts/build_ffmpeg_ios.sh

# Build iOS framework
xcodebuild -project WebMAlphaPlayer.xcodeproj -scheme WebMAlphaPlayer -configuration Release -sdk iphoneos
```

## Credits

Built with:
- [FFmpeg](https://ffmpeg.org/) - Video decoding
- [libvpx](https://www.webmproject.org/code/) - VP9 codec