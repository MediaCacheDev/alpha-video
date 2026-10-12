// swift-tools-version:5.9
import Foundation
import PackageDescription

// FFmpeg + libvpx as one static library per platform (scripts/build_xcframework.sh). A local
// build in prebuilt/ wins, so the package can be developed without a release.
let localFFmpegVPX = "prebuilt/FFmpegVPX.xcframework"
let packageDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let ffmpegVPX: Target = FileManager.default.fileExists(atPath: packageDir.appendingPathComponent(localFFmpegVPX).path)
    ? .binaryTarget(name: "FFmpegVPX", path: localFFmpegVPX)
    : .binaryTarget(
        name: "FFmpegVPX",
        url: "https://github.com/danielkpl2/alpha-video/releases/download/v1.0.0/FFmpegVPX.xcframework.zip",
        checksum: "7860611adec6ac8b92c1cf8bc161912d89ca70cb8c55e36e3f63115b7dd35fb4"
    )

// The target lives at the repo root, and SwiftPM takes resource-like files anywhere under a
// target as its resources (the FFmpeg sources in scripts/build, node_modules), so everything
// except cpp and ios/Sources is excluded. Listed from disk: excluding a missing path warns.
func entries(_ dir: String, except kept: Set<String>) -> [String] {
    let url = dir.isEmpty ? packageDir : packageDir.appendingPathComponent(dir)
    let names = (try? FileManager.default.contentsOfDirectory(atPath: url.path)) ?? []
    return names
        .filter { !$0.hasPrefix(".") && !kept.contains($0) }
        .map { dir.isEmpty ? $0 : "\(dir)/\($0)" }
}
let excluded = entries("", except: ["cpp", "ios", "Package.swift"])
    + entries("ios", except: ["Sources"])
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
            dependencies: ["FFmpegVPX"],
            path: ".",
            exclude: excluded,
            sources: ["cpp", "ios/Sources/AlphaVideo"],
            publicHeadersPath: "ios/Sources/AlphaVideo/include",
            cSettings: [.headerSearchPath("cpp")],
            cxxSettings: [.headerSearchPath("cpp"), .define("__STDC_CONSTANT_MACROS")],
            linkerSettings: [
                .linkedLibrary("z"),
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
            ]
        ),
        ffmpegVPX,
    ],
    cxxLanguageStandard: .cxx17
)
