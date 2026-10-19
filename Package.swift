// swift-tools-version:5.9
import Foundation
import PackageDescription

// Apps add AlphaVideoVPX (VP8, VP9), AlphaVideoAV1 (AV1) or both, and `import AlphaVideo` either way.
// The core alone isn't a product: iOS has no platform decoder for these files.
//
// Each codec library is a binary target (scripts/build_xcframework.sh). A local build in
// prebuilt/ wins, so the package can be developed without a release.
let packageDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
func codecLibrary(_ name: String, checksum: String) -> Target {
    let local = "prebuilt/\(name).xcframework"
    return FileManager.default.fileExists(atPath: packageDir.appendingPathComponent(local).path)
        ? .binaryTarget(name: name, path: local)
        : .binaryTarget(
            name: name,
            url: "https://github.com/MediaCacheDev/alpha-video/releases/download/v1.0.0/\(name).xcframework.zip",
            checksum: checksum
        )
}

// The targets live at the repo root, and SwiftPM takes resource-like files anywhere under a
// target as its resources (the codec sources in scripts/build, node_modules), so everything
// except cpp, codecs, ios/Sources, libwebm's parser and alpha-video-source's reader is excluded. Listed from disk: excluding a missing
// path warns.
func entries(_ dir: String, except kept: Set<String>) -> [String] {
    let url = dir.isEmpty ? packageDir : packageDir.appendingPathComponent(dir)
    let names = (try? FileManager.default.contentsOfDirectory(atPath: url.path)) ?? []
    return names
        .filter { !$0.hasPrefix(".") && !kept.contains($0) }
        .map { dir.isEmpty ? $0 : "\(dir)/\($0)" }
}
let webmParser = "third_party/libwebm/webm_parser"
// Range reading and storage, vendored by scripts/vendor_alpha_video_source.sh
let sourceReader = "third_party/alpha-video-source/src"
let excluded = entries("", except: ["cpp", "codecs", "ios", "third_party", "Package.swift"])
    + entries("ios", except: ["Sources"])
    + entries("third_party", except: ["libwebm", "alpha-video-source"])
    + entries("third_party/libwebm", except: ["webm_parser"])
    + entries("third_party/alpha-video-source", except: ["src"])
    + ["cpp/WebMAlphaPlayer_jni.cpp"]

// A codec module: its plugin adapter (codecs/<codec>) and the class AlphaVideoCodecs finds by name
// (ios/Sources/<target>), over the codec library. Rooted at the repo too, for cpp/'s plugin header.
func codecModule(_ name: String, codec: String, library: String) -> Target {
    .target(
        name: name,
        dependencies: ["AlphaVideo", .target(name: library)],
        path: ".",
        exclude: excluded,
        sources: ["codecs/\(codec)", "ios/Sources/\(name)"],
        publicHeadersPath: "ios/Sources/\(name)/include",
        cxxSettings: [.headerSearchPath("cpp"), .headerSearchPath("codecs/\(codec)")]
    )
}

let package = Package(
    name: "AlphaVideo",
    platforms: [.iOS(.v13)],
    products: [
        .library(name: "AlphaVideoVPX", targets: ["AlphaVideoVPX"]),
        .library(name: "AlphaVideoAV1", targets: ["AlphaVideoAV1"]),
    ],
    targets: [
        // One target for the C++ core and the Objective-C++ API: a separate core target would
        // have to make its C++ headers public, and Swift apps can't import a module with C++
        // headers unless they turn on C++ interop.
        .target(
            name: "AlphaVideo",
            path: ".",
            exclude: excluded,
            sources: ["cpp", "ios/Sources/AlphaVideo", webmParser, sourceReader],
            publicHeadersPath: "ios/Sources/AlphaVideo/include",
            cSettings: [.headerSearchPath("cpp")],
            cxxSettings: [
                .headerSearchPath("cpp"),
                .headerSearchPath(sourceReader),
                .headerSearchPath(webmParser),
                .headerSearchPath("\(webmParser)/include"),
            ],
            linkerSettings: [
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
            ]
        ),
        codecModule("AlphaVideoVPX", codec: "vpx", library: "AlphaVideoLibvpx"),
        codecModule("AlphaVideoAV1", codec: "av1", library: "AlphaVideoDav1d"),
        codecLibrary("AlphaVideoLibvpx", checksum: "0984d2040918cd2d518eb028f4f119e38dec1536fe00266c4b37b612cbcbeb37"),
        codecLibrary("AlphaVideoDav1d", checksum: "c3dac0b822a7da604ed8bdb4a89b3b99720731c7420e3c245c71781ac3e05c62"),
        // Each links one codec module, as an app would, and checks the core finds it alone
        .target(name: "AlphaVideoTestSupport", dependencies: ["AlphaVideo"], path: "tests/ios/Support"),
        .testTarget(name: "VPXOnlyTests", dependencies: ["AlphaVideoVPX", "AlphaVideoTestSupport"], path: "tests/ios/VPXOnlyTests"),
        .testTarget(name: "AV1OnlyTests", dependencies: ["AlphaVideoAV1", "AlphaVideoTestSupport"], path: "tests/ios/AV1OnlyTests"),
    ],
    cxxLanguageStandard: .cxx17
)
