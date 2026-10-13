require 'json'
require 'pathname'

package = JSON.parse(File.read(File.join(__dir__, 'package.json')))
pod_dir = Pathname.new(__dir__).relative_path_from(Pathname.new(Pod::Config.instance.installation_root)).to_s

Pod::Spec.new do |s|
  s.name         = 'react-native-alpha-video'
  s.version      = package['version']
  s.summary      = package['description']
  s.homepage     = package['homepage']
  s.license      = package['license']
  s.authors      = package['author']

  s.platforms    = { :ios => '13.0' }
  s.source       = { :git => package['repository']['url'], :tag => "v#{s.version}" }

  # The iOS library (ios/Sources/AlphaVideo, also the Swift package) and the React Native view
  s.source_files = [
    'ios/Sources/AlphaVideo/**/*.{h,mm}',
    'react-native/ios/**/*.{h,mm}',
    'cpp/**/*.{h,cpp}'
  ]
  s.exclude_files = [
    'cpp/*_jni.cpp'
  ]
  s.public_header_files = 'ios/Sources/AlphaVideo/include/AlphaVideo.h'
  
  # Prebuilt FFmpeg libraries, linked by the app (see user_target_xcconfig)
  s.preserve_paths = [
    'prebuilt/ios/**/*',
    'prebuilt/ios-simulator/**/*',
    'prebuilt/include/**/*'
  ]
  
  s.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'CLANG_CXX_LIBRARY' => 'libc++',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) __STDC_CONSTANT_MACROS',
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)/prebuilt/include" "$(PODS_TARGET_SRCROOT)/ios/Sources/AlphaVideo/include" "$(PODS_TARGET_SRCROOT)/cpp"',
    'LIBRARY_SEARCH_PATHS[sdk=iphoneos*]' => '"$(PODS_TARGET_SRCROOT)/prebuilt/ios/lib"',
    'LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*]' => '"$(PODS_TARGET_SRCROOT)/prebuilt/ios-simulator/lib"'
  }
  
  # Propagate linker flags to the user target (app). The prebuilt libs differ per SDK, so they
  # can't be vendored; the app links them by path, relative to the Podfile's directory.
  user_target_xcconfig = {}
  { 'iphoneos' => 'ios', 'iphonesimulator' => 'ios-simulator' }.each do |sdk, dir|
    lib_dir = "$(PODS_ROOT)/../#{pod_dir}/prebuilt/#{dir}/lib"
    user_target_xcconfig["LIBRARY_SEARCH_PATHS[sdk=#{sdk}*]"] = "$(inherited) \"#{lib_dir}\""
    user_target_xcconfig["OTHER_LDFLAGS[sdk=#{sdk}*]"] = '$(inherited) -lavformat -lavcodec -lavutil -lvpx'
  end
  s.user_target_xcconfig = user_target_xcconfig
  
  s.frameworks = ['Metal', 'MetalKit']
  s.libraries = ['z', 'c++']
  
  # React Native's helper: the Fabric and codegen dependencies, and RCT_NEW_ARCH_ENABLED on the
  # new architecture. It reads pod_target_xcconfig, so it comes after it.
  if respond_to?(:install_modules_dependencies, true)
    install_modules_dependencies(s)
  else
    s.dependency 'React-Core'
  end
end
