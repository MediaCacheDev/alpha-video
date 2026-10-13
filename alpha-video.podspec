# The iOS library without React Native, for CocoaPods. (React Native apps use
# react-native-alpha-video.podspec, which autolinking finds in the npm package.)
#
# The source is alpha-video-<version>-ios.zip from the GitHub Release (scripts/package_release.sh),
# because FFmpegVPX.xcframework isn't in git. 1.0.0 is on CocoaPods trunk, which is read-only from
# 2 December 2026. Later versions are used through the podspec at their tag:
#   pod 'alpha-video', :podspec => 'https://raw.githubusercontent.com/danielkpl2/alpha-video/v<version>/alpha-video.podspec'

Pod::Spec.new do |s|
  s.name         = 'alpha-video'
  s.module_name  = 'AlphaVideo'
  s.version      = '1.0.0'
  s.summary      = 'Transparent video for iOS: VP9 WebM with alpha, decoded by FFmpeg + libvpx and drawn with Metal.'
  s.homepage     = 'https://github.com/danielkpl2/alpha-video'
  s.license      = { :type => 'MIT', :file => 'LICENSE' }
  s.author       = { 'Daniel Kasprowicz' => '9932671+danielkpl2@users.noreply.github.com' }
  s.platform     = :ios, '13.0'
  s.source       = { :http => "https://github.com/danielkpl2/alpha-video/releases/download/v#{s.version}/alpha-video-#{s.version}-ios.zip",
                     :sha256 => 'aad3af2d4126c0f981d523acc5b732da7f6a3498274d490ec988d95086f694b4' }

  s.source_files        = 'cpp/*.{h,cpp}', 'ios/Sources/AlphaVideo/**/*.{h,mm}'
  s.public_header_files = 'ios/Sources/AlphaVideo/include/AlphaVideo.h'
  s.vendored_frameworks = 'FFmpegVPX.xcframework'
  s.frameworks          = 'Metal', 'MetalKit', 'UIKit'
  s.libraries           = 'z', 'c++'
  s.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) __STDC_CONSTANT_MACROS',
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)/cpp"'
  }
end
