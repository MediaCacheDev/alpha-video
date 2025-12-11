require 'json'

package = JSON.parse(File.read(File.join(__dir__, 'package.json')))

Pod::Spec.new do |s|
  s.name         = 'RNWebMAlphaPlayer'
  s.version      = package['version']
  s.summary      = package['description']
  s.homepage     = package['homepage']
  s.license      = package['license']
  s.authors      = package['author']

  s.platforms    = { :ios => '13.0' }
  s.source       = { :git => package['repository']['url'], :tag => "v#{s.version}" }

  s.source_files = [
    'ios/**/*.{h,m,mm}',
    'cpp/**/*.{h,hpp,c,cpp}'
  ]
  
  # Exclude Android JNI files
  s.exclude_files = [
    'cpp/*_jni.cpp',
    'cpp/*_jni.h'
  ]
  
  s.public_header_files = [
    'ios/**/*.h',
    'cpp/*.h'
  ]
  
  # Prebuilt FFmpeg libraries - keep but don't vendor (we'll link manually)
  s.preserve_paths = [
    'prebuilt/ios/**/*',
    'prebuilt/ios-simulator/**/*',
    'prebuilt/include/**/*'
  ]
  
  # Library search paths for device and simulator
  s.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'CLANG_CXX_LIBRARY' => 'libc++',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) __STDC_CONSTANT_MACROS',
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)/prebuilt/include"',
    'LIBRARY_SEARCH_PATHS[sdk=iphoneos*]' => '"$(PODS_TARGET_SRCROOT)/prebuilt/ios/lib"',
    'LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*]' => '"$(PODS_TARGET_SRCROOT)/prebuilt/ios-simulator/lib"'
  }
  
  # Propagate linker flags to the user target (app)
  s.user_target_xcconfig = {
    'LIBRARY_SEARCH_PATHS[sdk=iphoneos*]' => '$(inherited) "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios/lib"',
    'LIBRARY_SEARCH_PATHS[sdk=iphonesimulator*]' => '$(inherited) "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios-simulator/lib"',
    'OTHER_LDFLAGS[sdk=iphoneos*]' => '$(inherited) -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios/lib/libavcodec.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios/lib/libavformat.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios/lib/libavutil.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios/lib/libswscale.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios/lib/libswresample.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios/lib/libvpx.a"',
    'OTHER_LDFLAGS[sdk=iphonesimulator*]' => '$(inherited) -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios-simulator/lib/libavcodec.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios-simulator/lib/libavformat.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios-simulator/lib/libavutil.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios-simulator/lib/libswscale.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios-simulator/lib/libswresample.a" -force_load "$(PODS_ROOT)/../../node_modules/react-native-webm-alpha-player/prebuilt/ios-simulator/lib/libvpx.a"'
  }
  
  s.frameworks = ['Metal', 'MetalKit', 'AVFoundation', 'VideoToolbox', 'CoreMedia', 'CoreVideo', 'Security']
  s.libraries = ['z', 'bz2', 'iconv', 'c++']
  
  s.dependency 'React-Core'
end
