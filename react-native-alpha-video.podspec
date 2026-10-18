require 'json'
require 'pathname'

package = JSON.parse(File.read(File.join(__dir__, 'package.json')))
pod_dir = Pathname.new(__dir__).relative_path_from(Pathname.new(Pod::Config.instance.installation_root)).to_s

# The codec modules to build in, from the app's package.json as android/build.gradle reads them:
# "react-native-alpha-video": { "codecs": ["vpx"] }. Both without the setting.
codec_targets = { 'vpx' => 'AlphaVideoVPX', 'av1' => 'AlphaVideoAV1' }
app_package_json = File.join(Pod::Config.instance.installation_root, '..', 'package.json')
codecs_setting = File.exist?(app_package_json) &&
                 JSON.parse(File.read(app_package_json)).dig('react-native-alpha-video', 'codecs')
codecs = codec_targets.keys
if codecs_setting
  unless codecs_setting.is_a?(Array) && (codecs_setting - codec_targets.keys).empty?
    raise "react-native-alpha-video: \"codecs\" in #{app_package_json} must list some of #{codec_targets.keys}, not #{codecs_setting}"
  end
  codecs = codec_targets.keys & codecs_setting
end
# Once: CocoaPods evaluates a podspec several times per install
unless $react_native_alpha_video_codecs_reported
  $react_native_alpha_video_codecs_reported = true
  if codecs.empty?
    Pod::UI.warn 'react-native-alpha-video: no codecs, and iOS has no platform decoder for these files: every video reports NO_DECODER'
  else
    Pod::UI.puts "react-native-alpha-video: codecs #{codecs.join(', ')}"
  end
end

Pod::Spec.new do |s|
  s.name         = 'react-native-alpha-video'
  s.version      = package['version']
  s.summary      = package['description']
  s.homepage     = package['homepage']
  s.license      = package['license']
  s.authors      = package['author']

  s.platforms    = { :ios => '13.0' }
  s.source       = { :git => package['repository']['url'], :tag => "v#{s.version}" }

  # The iOS library (ios/Sources/AlphaVideo, also the Swift package), the React Native view, and
  # each codec module's plugin adapter and the class the core finds it by
  s.source_files = [
    'ios/Sources/AlphaVideo/**/*.{h,mm}',
    'react-native/ios/**/*.{h,mm}',
    'cpp/**/*.{h,cpp}',
    'third_party/libwebm/webm_parser/**/*.{h,cc}',
    'third_party/alpha-video-source/src/*.{h,cpp}'
  ] + codecs.flat_map { |codec| ["codecs/#{codec}/*.{h,cpp}", "ios/Sources/#{codec_targets[codec]}/**/*.{h,mm}"] }
  s.exclude_files = [
    'cpp/*_jni.cpp'
  ]
  s.public_header_files = 'ios/Sources/AlphaVideo/include/AlphaVideo.h'
  
  # Prebuilt libvpx and dav1d, linked by the app (see user_target_xcconfig)
  s.preserve_paths = [
    'prebuilt/ios/**/*',
    'prebuilt/ios-simulator/**/*',
    'prebuilt/include/**/*'
  ]
  
  header_paths = ['prebuilt/include', 'ios/Sources/AlphaVideo/include', 'cpp'] +
                 codecs.map { |codec| "codecs/#{codec}" } +
                 ['third_party/alpha-video-source/src', 'third_party/libwebm/webm_parser', 'third_party/libwebm/webm_parser/include']
  s.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'CLANG_CXX_LIBRARY' => 'libc++',
    'HEADER_SEARCH_PATHS' => header_paths.map { |path| "\"$(PODS_TARGET_SRCROOT)/#{path}\"" }.join(' '),
    'LIBRARY_SEARCH_PATHS[sdk=iphoneos*]' => '"$(PODS_TARGET_SRCROOT)/prebuilt/ios/lib"',
    'LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*]' => '"$(PODS_TARGET_SRCROOT)/prebuilt/ios-simulator/lib"'
  }
  
  # Propagate linker flags to the user target (app). The prebuilt libs differ per SDK, so they
  # can't be vendored; the app links them by path, relative to the Podfile's directory.
  # dav1d is force-loaded: an app that also links another dav1d (libavif's pod) would otherwise
  # bind the core's calls to whichever comes first on the link line, maybe one without 10-bit AV1.
  user_target_xcconfig = {}
  { 'iphoneos' => 'ios', 'iphonesimulator' => 'ios-simulator' }.each do |sdk, dir|
    lib_dir = "$(PODS_ROOT)/../#{pod_dir}/prebuilt/#{dir}/lib"
    flags = []
    flags << "-force_load \"#{lib_dir}/libdav1d.a\"" if codecs.include?('av1')
    flags << '-lvpx' if codecs.include?('vpx')
    next if flags.empty?
    user_target_xcconfig["LIBRARY_SEARCH_PATHS[sdk=#{sdk}*]"] = "$(inherited) \"#{lib_dir}\""
    user_target_xcconfig["OTHER_LDFLAGS[sdk=#{sdk}*]"] = "$(inherited) #{flags.join(' ')}"
  end
  s.user_target_xcconfig = user_target_xcconfig
  
  s.frameworks = ['Metal', 'MetalKit']
  s.libraries = ['c++']
  
  # React Native's helper: the Fabric and codegen dependencies, and RCT_NEW_ARCH_ENABLED on the
  # new architecture. It reads pod_target_xcconfig, so it comes after it.
  if respond_to?(:install_modules_dependencies, true)
    install_modules_dependencies(s)
  else
    s.dependency 'React-Core'
  end
end
