Pod::Spec.new do |s|
  s.name         = "WebMAlphaPlayer-iOS"
  s.version      = "1.0.0"
  s.summary      = "High-performance WebM VP9 video player with alpha transparency support for iOS"
  s.description  = <<-DESC
                   A standalone iOS framework for WebM VP9 video playback with alpha channel support.
                   Features native C++ core with Metal GPU acceleration for smooth 30fps playback.
                   DESC
  s.homepage     = "https://github.com/danielkpl2/webm-alpha-player-ios"
  s.license      = { :type => "LGPL-2.1", :file => "LICENSE" }
  s.author       = { "danielkpl2" => "danielkpl2@gmail.com" }
  s.platform     = :ios, "12.0"
  s.source       = { :git => "https://github.com/danielkpl2/webm-alpha-player-ios.git", :tag => "#{s.version}" }

  s.source_files = "Sources/WebMAlphaPlayer/**/*.{h,m,mm,cpp,hpp}"
  s.public_header_files = "Sources/WebMAlphaPlayer/include/*.h"

  # FFmpeg static libraries
  s.ios.vendored_libraries = "Libraries/ios/libavcodec.a", "Libraries/ios/libavformat.a", "Libraries/ios/libavutil.a", "Libraries/ios/libswscale.a", "Libraries/ios/libvpx.a"

  # Metal framework
  s.frameworks = "Metal", "MetalKit", "CoreVideo"

  # Compiler flags
  s.compiler_flags = "-std=c++17"

  # Deployment targets
  s.ios.deployment_target = "12.0"

  # Exclude simulator architectures for App Store
  s.pod_target_xcconfig = {
    'ONLY_ACTIVE_ARCH' => 'YES',
    'VALID_ARCHS' => 'arm64'
  }
end