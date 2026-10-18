// swift-tools-version:5.9
import Foundation
import PackageDescription

// libvpx + dav1d as one static library per platform (scripts/build_xcframework.sh). A local
// build in prebuilt/ wins, so the package can be developed without a release.
let localCodecs = "prebuilt/AlphaVideoCodecs.xcframework"
let packageDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let codecs: Target = FileManager.default.fileExists(atPath: packageDir.appendingPathComponent(localCodecs).path)
    ? .binaryTarget(name: "AlphaVideoCodecs", path: localCodecs)
    : .binaryTarget(
        name: "AlphaVideoCodecs",
        url: "https://github.com/MediaCacheDev/alpha-video/releases/download/v1.0.0/AlphaVideoCodecs.xcframework.zip",
        checksum: "8ea2461df7991d70ea62728b8141dfb6c89daff6f27b8007f51b1ed854effc11"
    )

// The target lives at the repo root, and SwiftPM takes resource-like files anywhere under a
// target as its resources (the codec sources in scripts/build, node_modules), so everything
// except cpp, ios/Sources, libwebm's parser and alpha-video-source's reader is excluded. Listed from disk: excluding a missing
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
let excluded = entries("", except: ["cpp", "ios", "third_party", "Package.swift"])
    + entries("ios", except: ["Sources"])
    + entries("third_party", except: ["libwebm", "alpha-video-source"])
    + entries("third_party/libwebm", except: ["webm_parser"])
    + entries("third_party/alpha-video-source", except: ["src"])
    + ["cpp/WebMAlphaPlayer_jni.cpp"]

let package = Package(
    name: "AlphaVideo",
    platforms: [.iOS(.v13)],
    products: [
        .library(name: "AlphaVideo", targets: ["AlphaVideo"]),
    ],
    targets: [
        // One target for the C++ core and the Objective-C++ API: a separate core target would
        // have to make its C++ headers public, and Swift apps can't import a module with C++
        // headers unless they turn on C++ interop.
        .target(
            name: "AlphaVideo",
            dependencies: ["AlphaVideoCodecs"],
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
        codecs,
    ],
    cxxLanguageStandard: .cxx17
)
