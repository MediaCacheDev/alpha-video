# The iOS library without React Native, for CocoaPods. (React Native apps use
# react-native-alpha-video.podspec, which autolinking finds in the npm package.)
#
#   pod 'alpha-video'                                   VP8 and VP9 (libvpx), AV1 (dav1d), range-source
#   pod 'alpha-video', :subspecs => ['VPX', 'Source']   VP8 and VP9 only
#   pod 'alpha-video', :subspecs => ['VPX', 'AV1']      without range-source
# alpha-video/Core alone plays nothing on iOS, which has no platform decoder for these files.
# Source is the range-source add-on: URLs read by range and kept on disk or in memory, and app
# data sources. Without it, the basic input: URLs downloaded whole.
#
# The source is alpha-video-<version>-ios.zip from the GitHub Release (scripts/package_release.sh),
# because the codec xcframeworks aren't in git. 1.0.0 is on CocoaPods trunk, which is read-only from
# 2 December 2026. Later versions are used through the podspec at their tag:
#   pod 'alpha-video', :podspec => 'https://raw.githubusercontent.com/MediaCacheDev/alpha-video/v<version>/alpha-video.podspec'

Pod::Spec.new do |s|
  s.name         = 'alpha-video'
  s.module_name  = 'AlphaVideo'
  s.version      = '1.0.0'
  s.summary      = 'Transparent video for iOS: VP8, VP9 or AV1 WebM with alpha, decoded by libvpx or dav1d and drawn with Metal.'
  s.homepage     = 'https://github.com/MediaCacheDev/alpha-video'
  s.license      = { :type => 'MIT', :file => 'LICENSE' }
  s.author       = { 'Daniel Kasprowicz' => '9932671+danielkpl2@users.noreply.github.com' }
  s.platform     = :ios, '13.0'
  s.source       = { :http => "https://github.com/MediaCacheDev/alpha-video/releases/download/v#{s.version}/alpha-video-#{s.version}-ios.zip",
                     :sha256 => '42cfc0297a74da447102fd1620f64a398b904661991467a675deb01adb83cd1c' }
  s.default_subspecs = 'VPX', 'AV1', 'Source'
  s.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)/cpp" "$(PODS_TARGET_SRCROOT)/codecs/vpx" "$(PODS_TARGET_SRCROOT)/codecs/av1" "$(PODS_TARGET_SRCROOT)/third_party/range-source/include" "$(PODS_TARGET_SRCROOT)/third_party/libwebm/webm_parser" "$(PODS_TARGET_SRCROOT)/third_party/libwebm/webm_parser/include"'
  }

  s.subspec 'Core' do |core|
    core.source_files        = 'cpp/*.{h,cpp}', 'ios/Sources/AlphaVideo/**/*.{h,mm}',
                               'third_party/libwebm/webm_parser/**/*.{h,cc}',
                               'third_party/range-source/include/*.h'
    core.exclude_files       = 'cpp/*_jni.cpp'
    core.public_header_files = 'ios/Sources/AlphaVideo/include/AlphaVideo.h'
    core.frameworks          = 'Metal', 'MetalKit', 'UIKit'
    core.libraries           = 'c++'
  end

  # The range-source add-on, found at runtime
  s.subspec 'Source' do |source|
    source.dependency 'alpha-video/Core'
    source.dependency 'range-source', '~> 1.0'
  end

  # Each codec module: its plugin adapter, the class the core finds by name, and the library
  s.subspec 'VPX' do |vpx|
    vpx.dependency 'alpha-video/Core'
    vpx.source_files        = 'codecs/vpx/*.{h,cpp}', 'ios/Sources/AlphaVideoVPX/**/*.{h,mm}'
    vpx.public_header_files = 'ios/Sources/AlphaVideoVPX/include/AlphaVideoVPX.h'
    vpx.vendored_frameworks = 'AlphaVideoLibvpx.xcframework'
  end

  s.subspec 'AV1' do |av1|
    av1.dependency 'alpha-video/Core'
    av1.source_files        = 'codecs/av1/*.{h,cpp}', 'ios/Sources/AlphaVideoAV1/**/*.{h,mm}'
    av1.public_header_files = 'ios/Sources/AlphaVideoAV1/include/AlphaVideoAV1.h'
    av1.vendored_frameworks = 'AlphaVideoDav1d.xcframework'
  end
end
