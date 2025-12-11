# Android WebM Alpha Player

A standalone Android library for high-performance WebM VP9 video playback with alpha transparency support.

## Features

- 🎬 **VP9 Alpha Transparency** - Full support for WebM videos with alpha channel
- ⚡ **High Performance** - Native C++ core with GPU-accelerated rendering
- 🤖 **OpenGL ES Rendering** - Hardware-accelerated YUVA→RGBA conversion
- 🔄 **Zero-Copy Pipeline** - Efficient memory usage with LRU frame caching
- 📱 **TextureView Compatible** - Works with ScrollView and proper z-ordering

## Installation

### Gradle

Add to your `build.gradle`:

```gradle
dependencies {
    implementation 'com.danielkpl2:webm-alpha-player-android:1.0.0'
}
```

### Manual

Download the AAR from releases and add to your `libs/` folder.

## Usage

### Basic Usage

```kotlin
import com.danielkpl2.webmalphaplayer.WebMAlphaTextureView

// In your layout XML
<WebMAlphaTextureView
    android:id="@+id/webmPlayer"
    android:layout_width="300dp"
    android:layout_height="300dp" />

// In your Activity/Fragment
val playerView = findViewById<WebMAlphaTextureView>(R.id.webmPlayer)

// Load and play
playerView.loadFromAsset("animation.webm")
playerView.play()
```

### Programmatic Creation

```kotlin
val playerView = WebMAlphaTextureView(context).apply {
    layoutParams = ViewGroup.LayoutParams(300, 300)
    loadFromUrl("https://example.com/video.webm")
    loop = true
    play()
}
container.addView(playerView)
```

## API Reference

### WebMAlphaTextureView

| Method | Description |
|--------|-------------|
| `loadFromAsset(assetName)` | Load from assets folder |
| `loadFromFile(filePath)` | Load from file path |
| `loadFromUrl(url)` | Load from HTTP/HTTPS URL |
| `play()` | Start playback |
| `pause()` | Pause playback |
| `stop()` | Stop and reset |
| `seekToFrame(frameIndex)` | Seek to specific frame |
| `setPlaybackSpeed(speed)` | Set playback speed (0.5x to 2.0x) |

### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `loop` | Boolean | true | Loop playback |
| `autoPlay` | Boolean | true | Auto-play when loaded |
| `playbackSpeed` | Float | 1.0f | Playback speed multiplier |

### Events

```kotlin
playerView.setPlayerListener(object : WebMAlphaTextureView.PlayerListener {
    override fun onLoad(totalFrames: Int, width: Int, height: Int) {
        // Video loaded successfully
    }

    override fun onPlay() {
        // Playback started
    }

    override fun onPause() {
        // Playback paused
    }

    override fun onEnd() {
        // Playback finished
    }

    override fun onError(error: String) {
        // Error occurred
    }
})
```

## Requirements

- **Android API**: 21+ (Android 5.0)
- **Architecture**: arm64-v8a, armeabi-v7a, x86_64
- **OpenGL ES**: 2.0+

## Technical Details

### Architecture

```
┌─────────────────┐    ┌─────────────────┐
│   Android App   │    │   JNI Bridge    │
│   Kotlin/Java   │◄──►│   WebMAlphaPlayer│
│   Activity      │    │   (C++)         │
└─────────────────┘    └─────────────────┘
         │                       │
         ▼                       ▼
┌─────────────────┐    ┌─────────────────┐
│   TextureView   │    │   OpenGL ES     │
│   Surface       │    │   YUVA→RGBA     │
│   Rendering     │    │   GPU Shader    │
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
# Install Android NDK
# Install FFmpeg build tools
```

### Build Commands

```bash
# Build FFmpeg for Android
./scripts/build_ffmpeg_android.sh

# Build Android library
./gradlew assembleRelease
```

## Credits

Built with:
- [FFmpeg](https://ffmpeg.org/) - Video decoding
- [libvpx](https://www.webmproject.org/code/) - VP9 codec