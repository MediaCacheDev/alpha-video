/**
 * WebMAlphaPlayer JNI Bridge
 * Connects Java/Kotlin to C++ WebMAlphaPlayer core
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "WebMAlphaPlayer.h"
#include "FileRangeIO.h"
#include "GrowingFile.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <string>

#define LOG_TAG "WebMAlphaPlayer"
// JNI lifecycle traces, compiled in only with WEBM_DEBUG_LOGS
#if defined(WEBM_DEBUG_LOGS)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do {} while (0)
#endif
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Store player instances by ID
// Using shared_ptr to allow safe reference during async operations
std::unordered_map<jlong, std::shared_ptr<webm::WebMAlphaPlayer>> players;
std::mutex playersMutex;
jlong nextPlayerId = 1;

// Calls that can block or emit callbacks must run outside playersMutex: a preload holds the
// decode lock while it emits events, and the "loaded" event takes playersMutex under
// callbacksMutex, so holding playersMutex across them deadlocks.
std::shared_ptr<webm::WebMAlphaPlayer> findPlayer(jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return it != players.end() ? it->second : nullptr;
}

// Track which players are destroyed (to reject callbacks from destroyed players)
std::unordered_set<jlong> destroyedPlayers;
std::mutex destroyedMutex;

// Track which players are stopped/unloaded (to reject callbacks during soft-release)
// This is separate from destroyedPlayers because stopped players can be re-started
std::unordered_set<jlong> stoppedPlayers;
std::mutex stoppedMutex;

// JNI callback references
struct CallbackRefs {
    JavaVM* jvm = nullptr;
    jobject listener = nullptr;
    jmethodID onLoad = nullptr;
    jmethodID onFrame = nullptr;
    jmethodID onPlay = nullptr;
    jmethodID onPause = nullptr;
    jmethodID onStop = nullptr;
    jmethodID onLoop = nullptr;
    jmethodID onEnd = nullptr;
    jmethodID onError = nullptr;
    jmethodID onPreloadComplete = nullptr;
};

std::unordered_map<jlong, CallbackRefs> callbacks;
std::mutex callbacksMutex;

JNIEnv* getJNIEnv(JavaVM* jvm) {
    if (!jvm) {
        LOGE("getJNIEnv called with null jvm!");
        return nullptr;
    }
    JNIEnv* env = nullptr;
    jint result = jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("Failed to attach thread to JVM");
            return nullptr;
        }
    } else if (result != JNI_OK) {
        LOGE("GetEnv failed with result %d", result);
        return nullptr;
    }
    return env;
}

std::string stringFromJava(JNIEnv* env, jstring value) {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

} // anonymous namespace

extern "C" {

//==============================================================================
// Player Lifecycle
//==============================================================================

JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeCreate(
    JNIEnv* env,
    jclass clazz,
    jint decodeThreads,
    jint cacheSize,
    jboolean preloadAllFrames,
    jboolean loopPlayback,
    jdouble playbackSpeed,
    jstring modeStr,
    jint streamCacheSize,
    jint streamPrefetchAhead
) {
    auto player = std::make_shared<webm::WebMAlphaPlayer>();
    
    // Parse mode string
    webm::PlaybackMode mode = webm::PlaybackMode::Preload;
    if (modeStr != nullptr) {
        const char* modeChars = env->GetStringUTFChars(modeStr, nullptr);
        if (modeChars) {
            std::string modeString(modeChars);
            if (modeString == "stream") {
                mode = webm::PlaybackMode::Stream;
            } else if (modeString == "streamAndPreload") {
                mode = webm::PlaybackMode::StreamAndPreload;
            }
            env->ReleaseStringUTFChars(modeStr, modeChars);
        }
    }
    
    webm::PlayerConfig config;
    config.decodeThreads = decodeThreads;
    config.cacheSize = cacheSize;
    config.preloadAllFrames = preloadAllFrames;
    config.loopPlayback = loopPlayback;
    config.playbackSpeed = playbackSpeed;
    config.mode = mode;
    config.stream = (mode == webm::PlaybackMode::Stream);  // Legacy compatibility
    config.streamCacheSize = streamCacheSize > 0 ? streamCacheSize : 30;
    config.streamPrefetchAhead = streamPrefetchAhead > 0 ? streamPrefetchAhead : 15;
    player->setConfig(config);
    
    std::lock_guard<std::mutex> lock(playersMutex);
    jlong id = nextPlayerId++;
    players[id] = player;
    
    // Remove from destroyed/stopped sets if ID was reused (shouldn't happen, but be safe)
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        destroyedPlayers.erase(id);
    }
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(id);
    }
    
    LOGI("Created player %lld (threads=%d, cache=%d)", id, decodeThreads, cacheSize);
    return id;
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeDestroy(
    JNIEnv* env,
    jclass clazz,
    jlong playerId
) {
    // Mark as destroyed FIRST so callbacks are rejected immediately
    {
        std::lock_guard<std::mutex> lock(destroyedMutex);
        destroyedPlayers.insert(playerId);
    }
    
    // Also mark as stopped for good measure
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.insert(playerId);
    }
    
    // Get shared_ptr to stop the player WITHOUT holding playersMutex
    // stop() can block waiting for threads to join
    {
        std::shared_ptr<webm::WebMAlphaPlayer> player;
        {
            std::lock_guard<std::mutex> lock(playersMutex);
            auto it = players.find(playerId);
            if (it != players.end()) {
                player = it->second;  // Increment refcount
            }
        }
        // Call stop() without holding playersMutex
        if (player) {
            player->stop();  // Signal to stop any ongoing decode/preload
        }
    }
    
    // Clean up callbacks
    {
        std::lock_guard<std::mutex> lock(callbacksMutex);
        auto it = callbacks.find(playerId);
        if (it != callbacks.end()) {
            if (it->second.listener) {
                env->DeleteGlobalRef(it->second.listener);
            }
            callbacks.erase(it);
        }
    }
    
    // Remove player (shared_ptr may still be held by in-flight operations, but that's OK)
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        players.erase(playerId);
    }
    
    // Clean up stopped set entry
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(playerId);
    }
    
    LOGI("Destroyed player %lld", playerId);
}

//==============================================================================
// Callbacks
//==============================================================================

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeSetListener(
    JNIEnv* env,
    jclass clazz,
    jlong playerId,
    jobject listener
) {
    auto player = findPlayer(playerId);
    if (!player) return;
    
    CallbackRefs refs;
    env->GetJavaVM(&refs.jvm);
    
    if (listener) {
        refs.listener = env->NewGlobalRef(listener);
        if (!refs.listener) {
            LOGE("Failed to create global ref for listener");
            return;
        }
        
        jclass listenerClass = env->GetObjectClass(listener);
        if (!listenerClass) {
            LOGE("Failed to get listener class");
            env->DeleteGlobalRef(refs.listener);
            return;
        }
        
        refs.onLoad = env->GetMethodID(listenerClass, "onLoad", "(III)V");
        refs.onFrame = env->GetMethodID(listenerClass, "onFrame", "(ILjava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;II)V");
        refs.onPlay = env->GetMethodID(listenerClass, "onPlay", "()V");
        refs.onPause = env->GetMethodID(listenerClass, "onPause", "()V");
        refs.onStop = env->GetMethodID(listenerClass, "onStop", "()V");
        refs.onLoop = env->GetMethodID(listenerClass, "onLoop", "()V");
        refs.onEnd = env->GetMethodID(listenerClass, "onEnd", "()V");
        refs.onError = env->GetMethodID(listenerClass, "onError", "(Ljava/lang/String;)V");
        refs.onPreloadComplete = env->GetMethodID(listenerClass, "onPreloadComplete", "(ID)V");
    }
    
    {
        std::lock_guard<std::mutex> clock(callbacksMutex);
        // Remove old listener if exists
        auto oldIt = callbacks.find(playerId);
        if (oldIt != callbacks.end() && oldIt->second.listener) {
            env->DeleteGlobalRef(oldIt->second.listener);
        }
        callbacks[playerId] = refs;
    }
    
    // Set up C++ callbacks
    player->setFrameCallback([playerId](std::shared_ptr<webm::YUVAFrame> frame) {
        // Check if player was destroyed
        {
            std::lock_guard<std::mutex> dlock(destroyedMutex);
            if (destroyedPlayers.count(playerId)) return;
        }
        
        // Check if player was stopped (soft-release)
        {
            std::lock_guard<std::mutex> slock(stoppedMutex);
            if (stoppedPlayers.count(playerId)) return;
        }
        
        std::lock_guard<std::mutex> lock(callbacksMutex);
        auto it = callbacks.find(playerId);
        if (it == callbacks.end() || !it->second.listener) return;
        
        JNIEnv* env = getJNIEnv(it->second.jvm);
        if (!env) return;
        
        // Validate frame data before creating ByteBuffers
        if (!frame || !frame->yPlane || !frame->uPlane || !frame->vPlane || !frame->aPlane ||
            frame->ySize == 0 || frame->uSize == 0 || frame->vSize == 0 || frame->aSize == 0) {
            LOGE("Invalid frame data in callback for player %lld", playerId);
            return;
        }
        
        // Validate method ID before calling
        if (!it->second.onFrame) {
            LOGE("onFrame method ID is null for player %lld", playerId);
            return;
        }
        
        // Create direct ByteBuffers pointing to frame data (zero-copy!)
        jobject yBuffer = env->NewDirectByteBuffer(frame->yPlane, frame->ySize);
        jobject uBuffer = env->NewDirectByteBuffer(frame->uPlane, frame->uSize);
        jobject vBuffer = env->NewDirectByteBuffer(frame->vPlane, frame->vSize);
        jobject aBuffer = env->NewDirectByteBuffer(frame->aPlane, frame->aSize);
        
        // Check if ByteBuffer creation succeeded
        if (!yBuffer || !uBuffer || !vBuffer || !aBuffer) {
            LOGE("Failed to create ByteBuffers for player %lld", playerId);
            if (yBuffer) env->DeleteLocalRef(yBuffer);
            if (uBuffer) env->DeleteLocalRef(uBuffer);
            if (vBuffer) env->DeleteLocalRef(vBuffer);
            if (aBuffer) env->DeleteLocalRef(aBuffer);
            return;
        }
        
        env->CallVoidMethod(
            it->second.listener,
            it->second.onFrame,
            frame->frameIndex,
            yBuffer, uBuffer, vBuffer, aBuffer,
            frame->width, frame->height
        );
        
        // Check for Java exception
        if (env->ExceptionCheck()) {
            LOGE("Java exception occurred in onFrame for player %lld", playerId);
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        
        env->DeleteLocalRef(yBuffer);
        env->DeleteLocalRef(uBuffer);
        env->DeleteLocalRef(vBuffer);
        env->DeleteLocalRef(aBuffer);
    });
    
    player->setEventCallback([playerId](const std::string& event, const std::string& data) {
        // Check if player was destroyed
        {
            std::lock_guard<std::mutex> dlock(destroyedMutex);
            if (destroyedPlayers.count(playerId)) return;
        }
        
        // Check if player was stopped (soft-release) - but allow stop event through
        if (event != "stop") {
            std::lock_guard<std::mutex> slock(stoppedMutex);
            if (stoppedPlayers.count(playerId)) return;
        }
        
        std::lock_guard<std::mutex> lock(callbacksMutex);
        auto it = callbacks.find(playerId);
        if (it == callbacks.end() || !it->second.listener) return;
        
        JNIEnv* env = getJNIEnv(it->second.jvm);
        if (!env) return;
        
        if (event == "loaded") {
            // Parse frame count from data
            int frames = std::stoi(data.substr(0, data.find(' ')));
            // Get width/height without holding playersMutex during Java call
            // This prevents deadlock when main thread holds playersMutex and waits for decodeMutex_
            int width = 0, height = 0;
            {
                std::lock_guard<std::mutex> plock(playersMutex);
                auto playerIt = players.find(playerId);
                if (playerIt != players.end()) {
                    width = playerIt->second->getWidth();
                    height = playerIt->second->getHeight();
                } else {
                    return;  // Player gone
                }
            }
            // Now call Java without holding playersMutex
            env->CallVoidMethod(it->second.listener, it->second.onLoad, frames, width, height);
        } else if (event == "play") {
            env->CallVoidMethod(it->second.listener, it->second.onPlay);
        } else if (event == "pause") {
            env->CallVoidMethod(it->second.listener, it->second.onPause);
        } else if (event == "stop") {
            env->CallVoidMethod(it->second.listener, it->second.onStop);
        } else if (event == "loop") {
            env->CallVoidMethod(it->second.listener, it->second.onLoop);
        } else if (event == "ended") {
            env->CallVoidMethod(it->second.listener, it->second.onEnd);
        } else if (event == "error") {
            jstring errStr = env->NewStringUTF(data.c_str());
            env->CallVoidMethod(it->second.listener, it->second.onError, errStr);
            env->DeleteLocalRef(errStr);
        } else if (event == "preload_complete") {
            // "X frames in Yms". Parsed without std::stod, whose exception would abort the app.
            int frames = std::atoi(data.c_str());
            size_t in = data.find(" in ");
            double ms = in != std::string::npos ? std::atof(data.c_str() + in + 4) : 0.0;
            env->CallVoidMethod(it->second.listener, it->second.onPreloadComplete, frames, ms);
        }
    });
    
    LOGI("Set listener for player %lld", playerId);
}

//==============================================================================
// Loading
//==============================================================================

JNIEXPORT jboolean JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeLoadFromFile(
    JNIEnv* env,
    jclass clazz,
    jlong playerId,
    jstring path
) {
    // Check if already destroyed
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) return JNI_FALSE;
    }
    
    // Remove from stopped set when loading new content (re-enabling callbacks)
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(playerId);
    }
    
    // Get shared_ptr to keep player alive during operation
    std::shared_ptr<webm::WebMAlphaPlayer> player;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return JNI_FALSE;
        player = it->second;  // Increment refcount
    }
    
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    std::string pathCopy(pathStr);  // Copy before releasing
    env->ReleaseStringUTFChars(path, pathStr);
    
    // Check again if destroyed before expensive operation
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) {
            LOGI("LoadFromFile aborted - player %lld destroyed", playerId);
            return JNI_FALSE;
        }
    }
    
    bool result = player->loadFromFile(pathCopy);
    
    LOGI("LoadFromFile %s: %s", pathCopy.c_str(), result ? "success" : "failed");
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeLoadFromFd(
    JNIEnv* env,
    jclass clazz,
    jlong playerId,
    jint fd,
    jlong offset,
    jlong length
) {
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) return JNI_FALSE;
    }
    
    // Remove from stopped set when loading new content (re-enabling callbacks)
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(playerId);
    }
    
    std::shared_ptr<webm::WebMAlphaPlayer> player;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return JNI_FALSE;
        player = it->second;
    }
    
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) {
            LOGI("LoadFromFd aborted - player %lld destroyed", playerId);
            return JNI_FALSE;
        }
    }
    
    bool result = player->loadFromFileRange(fd, offset, length);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeLoadFromDownload(
    JNIEnv* env,
    jclass clazz,
    jlong playerId,
    jstring path
) {
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) return JNI_FALSE;
    }
    
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(playerId);
    }
    
    std::shared_ptr<webm::WebMAlphaPlayer> player;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return JNI_FALSE;
        player = it->second;
    }
    
    std::string pathCopy = stringFromJava(env, path);
    
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) {
            LOGI("LoadFromDownload aborted - player %lld destroyed", playerId);
            return JNI_FALSE;
        }
    }
    
    bool result = player->loadFromDownload(pathCopy);
    return result ? JNI_TRUE : JNI_FALSE;
}

//==============================================================================
// Downloads into the cache (WebMDownloader), readable while they run
//==============================================================================

JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_WebMDownloader_nativeBegin(
    JNIEnv* env,
    jclass clazz,
    jstring path,
    jstring partPath,
    jlong expectedSize
) {
    auto file = webm::GrowingFile::begin(stringFromJava(env, path), stringFromJava(env, partPath), expectedSize);
    if (!file) return 0;
    return reinterpret_cast<jlong>(new std::shared_ptr<webm::GrowingFile>(std::move(file)));
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMDownloader_nativeSetWritten(
    JNIEnv* env,
    jclass clazz,
    jlong handle,
    jlong bytes
) {
    (*reinterpret_cast<std::shared_ptr<webm::GrowingFile>*>(handle))->setWritten(bytes);
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMDownloader_nativeEnd(
    JNIEnv* env,
    jclass clazz,
    jlong handle,
    jboolean finished
) {
    auto* file = reinterpret_cast<std::shared_ptr<webm::GrowingFile>*>(handle);
    if (finished) {
        (*file)->finish();
    } else {
        (*file)->fail();
    }
    delete file;
}

JNIEXPORT jboolean JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeLoadFromBytes(
    JNIEnv* env,
    jclass clazz,
    jlong playerId,
    jbyteArray data
) {
    // Check if already destroyed
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) return JNI_FALSE;
    }
    
    // Remove from stopped set when loading new content (re-enabling callbacks)
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(playerId);
    }
    
    // Get shared_ptr to keep player alive during operation
    std::shared_ptr<webm::WebMAlphaPlayer> player;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return JNI_FALSE;
        player = it->second;  // Increment refcount
    }
    
    jsize length = env->GetArrayLength(data);
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    
    // Copy data since we can't hold JNI array reference across potential GC
    std::vector<uint8_t> dataCopy(reinterpret_cast<const uint8_t*>(bytes),
                                   reinterpret_cast<const uint8_t*>(bytes) + length);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    
    // Check again if destroyed before expensive operation
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) {
            LOGI("LoadFromBytes aborted - player %lld destroyed", playerId);
            return JNI_FALSE;
        }
    }
    
    bool result = player->loadFromMemory(dataCopy.data(), dataCopy.size());
    
    LOGI("LoadFromBytes %d bytes: %s", (int)length, result ? "success" : "failed");
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeUnload(
    JNIEnv* env,
    jclass clazz,
    jlong playerId
) {
    // Get shared_ptr WITHOUT holding playersMutex during unload()
    // unload() can block waiting for decodeMutex_, which could cause deadlock
    // if another thread's event callback needs playersMutex
    std::shared_ptr<webm::WebMAlphaPlayer> player;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return;
        player = it->second;  // Increment refcount
    }
    // Now call unload() without holding playersMutex
    player->unload();
}

//==============================================================================
// Playback Control
//==============================================================================

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativePlay(JNIEnv* env, jclass clazz, jlong playerId) {
    // Remove from stopped set when playing resumes
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(playerId);
    }
    
    if (auto player = findPlayer(playerId)) player->play();
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativePause(JNIEnv* env, jclass clazz, jlong playerId) {
    if (auto player = findPlayer(playerId)) player->pause();
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeStop(JNIEnv* env, jclass clazz, jlong playerId) {
    // Mark as stopped FIRST to block callbacks immediately
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.insert(playerId);
    }
    
    // Get shared_ptr WITHOUT holding playersMutex during stop()
    // stop() joins the playback thread which could take time
    std::shared_ptr<webm::WebMAlphaPlayer> player;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return;
        player = it->second;  // Increment refcount
    }
    // Now call stop() without holding playersMutex
    player->stop();
    
    LOGI("Stopped player %lld (callbacks blocked)", playerId);
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeSeekToFrame(
    JNIEnv* env, jclass clazz, jlong playerId, jint frameIndex
) {
    if (auto player = findPlayer(playerId)) player->seekToFrame(frameIndex);
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeSetPlaybackSpeed(
    JNIEnv* env, jclass clazz, jlong playerId, jdouble speed
) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    if (it != players.end()) it->second->setPlaybackSpeed(speed);
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeSetVerbose(
    JNIEnv* env, jclass clazz, jlong playerId, jboolean verbose
) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    if (it != players.end()) it->second->setVerbose(verbose == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeSeekToTime(
    JNIEnv* env, jclass clazz, jlong playerId, jdouble seconds
) {
    if (auto player = findPlayer(playerId)) player->seekTo(seconds);
}

//==============================================================================
// State Queries
//==============================================================================

JNIEXPORT jboolean JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeIsLoaded(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end() && it->second->isLoaded()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeIsPlaying(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end() && it->second->isPlaying()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetCurrentFrame(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getCurrentFrame() : 0;
}

JNIEXPORT jint JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetTotalFrames(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getTotalFrames() : 0;
}

JNIEXPORT jint JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetWidth(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getWidth() : 0;
}

JNIEXPORT jint JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetHeight(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getHeight() : 0;
}

JNIEXPORT jfloatArray JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetYuvToRgb(JNIEnv* env, jclass clazz, jlong playerId) {
    webm::YuvToRgb m;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return nullptr;
        m = it->second->getYuvToRgb();
    }
    jfloatArray out = env->NewFloatArray(16);
    if (out) env->SetFloatArrayRegion(out, 0, 16, m.m);
    return out;
}

JNIEXPORT jdouble JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetDuration(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getDuration() : 0.0;
}

JNIEXPORT jdouble JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetFrameRate(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getFrameRate() : 0.0;
}

//==============================================================================
// Direct Frame Access
//==============================================================================

JNIEXPORT jobject JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetFrame(
    JNIEnv* env, jclass clazz, jlong playerId, jint frameIndex
) {
    // Check if destroyed first
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) return nullptr;
    }
    
    auto player = findPlayer(playerId);
    if (!player) return nullptr;
    std::shared_ptr<webm::YUVAFrame> frame = player->getFrame(frameIndex);
    if (!frame) return nullptr;
    
    // Now we have a shared_ptr keeping the frame alive.
    // CRITICAL: Create Java-owned byte arrays and copy data!
    // The C++ frame could be freed when the cache is cleared or player destroyed.
    
    // Create Java byte arrays and copy data
    jbyteArray yArray = env->NewByteArray(frame->ySize);
    jbyteArray uArray = env->NewByteArray(frame->uSize);
    jbyteArray vArray = env->NewByteArray(frame->vSize);
    jbyteArray aArray = env->NewByteArray(frame->aSize);
    
    if (!yArray || !uArray || !vArray || !aArray) {
        LOGE("Failed to allocate byte arrays for frame %d", frameIndex);
        return nullptr;
    }
    
    // Copy data from C++ frame to Java arrays
    env->SetByteArrayRegion(yArray, 0, frame->ySize, reinterpret_cast<const jbyte*>(frame->yPlane));
    env->SetByteArrayRegion(uArray, 0, frame->uSize, reinterpret_cast<const jbyte*>(frame->uPlane));
    env->SetByteArrayRegion(vArray, 0, frame->vSize, reinterpret_cast<const jbyte*>(frame->vPlane));
    env->SetByteArrayRegion(aArray, 0, frame->aSize, reinterpret_cast<const jbyte*>(frame->aPlane));
    
    // Wrap byte arrays in ByteBuffers
    jclass byteBufferClass = env->FindClass("java/nio/ByteBuffer");
    jmethodID wrapMethod = env->GetStaticMethodID(byteBufferClass, "wrap", "([B)Ljava/nio/ByteBuffer;");
    
    jobject yBuffer = env->CallStaticObjectMethod(byteBufferClass, wrapMethod, yArray);
    jobject uBuffer = env->CallStaticObjectMethod(byteBufferClass, wrapMethod, uArray);
    jobject vBuffer = env->CallStaticObjectMethod(byteBufferClass, wrapMethod, vArray);
    jobject aBuffer = env->CallStaticObjectMethod(byteBufferClass, wrapMethod, aArray);
    
    // Find the WebMFrame class
    jclass frameClass = env->FindClass("dev/mediacache/alphavideo/WebMFrame");
    if (!frameClass) return nullptr;
    
    // Create instance with constructor
    jmethodID constructor = env->GetMethodID(frameClass, "<init>", 
        "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;IIID)V");
    
    jobject frameObj = env->NewObject(frameClass, constructor,
        yBuffer, uBuffer, vBuffer, aBuffer,
        frame->width, frame->height, frame->frameIndex,
        frame->getDurationMs() / 1000.0);
    
    // Clean up local refs (byte arrays are now referenced by ByteBuffers)
    env->DeleteLocalRef(yBuffer);
    env->DeleteLocalRef(uBuffer);
    env->DeleteLocalRef(vBuffer);
    env->DeleteLocalRef(aBuffer);
    env->DeleteLocalRef(yArray);
    env->DeleteLocalRef(uArray);
    env->DeleteLocalRef(vArray);
    env->DeleteLocalRef(aArray);
    env->DeleteLocalRef(byteBufferClass);
    
    return frameObj;
}

//==============================================================================
// Statistics
//==============================================================================

JNIEXPORT jlongArray JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetStats(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    if (it == players.end()) return nullptr;
    
    auto stats = it->second->getStats();
    
    // Return as long array: [framesDecoded, cacheHits, cacheMisses, decodeTimeUs]
    jlongArray result = env->NewLongArray(4);
    jlong values[] = {
        stats.framesDecoded,
        stats.cacheHits,
        stats.cacheMisses,
        stats.decodeTimeUs
    };
    env->SetLongArrayRegion(result, 0, 4, values);
    
    return result;
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeResetStats(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    if (it != players.end()) {
        it->second->resetStats();
    }
}

//==============================================================================
// FFmpeg Info
//==============================================================================

JNIEXPORT jstring JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetFFmpegVersion(JNIEnv* env, jclass clazz) {
    std::string version = webm::WebMAlphaPlayer::getFFmpegVersion();
    return env->NewStringUTF(version.c_str());
}

//==============================================================================
// Dual-Stream Demuxer for MediaCodec Hardware Decoding
// Extracts raw VP9/AV1 packets from dual-stream WebM files
//==============================================================================

// Include FFmpeg headers for demuxer
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace {

// Check if a codec is supported for dual-stream hardware decode
static bool isSupportedVideoCodec(AVCodecID id) {
    return id == AV_CODEC_ID_VP9 || id == AV_CODEC_ID_AV1;
}

/**
 * Demuxer context for dual-stream WebM files.
 * Extracts VP9/AV1 packets without decoding - MediaCodec will decode them.
 */
struct DualStreamDemuxer {
    AVFormatContext* formatCtx = nullptr;
    int colorStreamIndex = -1;
    int alphaStreamIndex = -1;
    int width = 0;
    int height = 0;
    int totalFrames = 0;
    double frameRate = 30.0;
    double timeBase = 0.0;
    AVCodecID codecId = AV_CODEC_ID_NONE;
    // AVColorRange and AVColorSpace of each stream, for MediaCodec's colour keys
    int colorRange = AVCOL_RANGE_UNSPECIFIED, colorSpace = AVCOL_SPC_UNSPECIFIED;
    int alphaRange = AVCOL_RANGE_UNSPECIFIED, alphaSpace = AVCOL_SPC_UNSPECIFIED;
    
    // Track frame indices for each stream
    int colorFrameIndex = 0;
    int alphaFrameIndex = 0;
    
    AVIOContext* io = nullptr;  // Owned; must outlive formatCtx
    // Set on destroy so a read waiting on a download returns
    std::atomic<bool> aborted{false};
    
    ~DualStreamDemuxer() {
        if (formatCtx) {
            avformat_close_input(&formatCtx);
        }
        webm::closeFileRangeIO(&io);
    }
};

// Opens *io (already created) as a demuxer input. On failure *io is closed and nothing is left to free.
static bool openInput(AVFormatContext** formatCtx, AVIOContext** io) {
    *formatCtx = avformat_alloc_context();
    if (!*formatCtx) {
        webm::closeFileRangeIO(io);
        return false;
    }
    (*formatCtx)->pb = *io;
    (*formatCtx)->flags |= AVFMT_FLAG_CUSTOM_IO;
    
    int ret = avformat_open_input(formatCtx, nullptr, nullptr, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        LOGE("Failed to open video: %s", err);
        webm::closeFileRangeIO(io);
        return false;
    }
    return true;
}

// Opens bytes [offset, offset + length) of fd as a demuxer input. On failure nothing is left to free.
static bool openRangeInput(int fd, jlong offset, jlong length, AVFormatContext** formatCtx, AVIOContext** io) {
    *io = webm::openFileRangeIO(fd, offset, length);
    if (!*io) {
        LOGE("Failed to open fd %d range (offset %lld, length %lld)", fd, (long long)offset, (long long)length);
        return false;
    }
    return openInput(formatCtx, io);
}

// Opens a cache file that may still be downloading. On failure nothing is left to free.
static bool openDownloadInput(const std::string& path, const std::atomic<bool>* abort,
                              AVFormatContext** formatCtx, AVIOContext** io) {
    *io = webm::openDownloadIO(path, abort);
    if (!*io) {
        LOGE("Failed to open %s", path.c_str());
        return false;
    }
    return openInput(formatCtx, io);
}

// Whether the opened input holds 2+ VP9/AV1 video streams. Closes the input.
static bool hasDualVideoStreams(AVFormatContext* formatCtx, AVIOContext* io) {
    bool dual = false;
    if (avformat_find_stream_info(formatCtx, nullptr) >= 0) {
        int videoStreamCount = 0;
        for (unsigned i = 0; i < formatCtx->nb_streams; i++) {
            AVStream* stream = formatCtx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                isSupportedVideoCodec(stream->codecpar->codec_id)) {
                videoStreamCount++;
            }
        }
        dual = videoStreamCount >= 2;
    }
    avformat_close_input(&formatCtx);
    webm::closeFileRangeIO(&io);
    return dual;
}

// Shared so that destroying a demuxer while its demux thread waits on a download can't free it
// under that read; the read's own reference frees it once it returns.
std::unordered_map<jlong, std::shared_ptr<DualStreamDemuxer>> demuxers;
std::mutex demuxersMutex;
jlong nextDemuxerId = 1;

std::shared_ptr<DualStreamDemuxer> findDemuxer(jlong demuxerId) {
    std::lock_guard<std::mutex> lock(demuxersMutex);
    auto it = demuxers.find(demuxerId);
    return it == demuxers.end() ? nullptr : it->second;
}

// Reads the stream layout of an opened demuxer and registers it. Returns its id, or 0.
jlong registerDemuxer(std::shared_ptr<DualStreamDemuxer> demuxer) {
    int ret = avformat_find_stream_info(demuxer->formatCtx, nullptr);
    if (ret < 0) {
        LOGE("[Buffered] Failed to find stream info");
        return 0;
    }
    
    for (unsigned i = 0; i < demuxer->formatCtx->nb_streams; i++) {
        AVStream* stream = demuxer->formatCtx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            isSupportedVideoCodec(stream->codecpar->codec_id)) {
            if (demuxer->colorStreamIndex < 0) {
                demuxer->colorStreamIndex = i;
                demuxer->width = stream->codecpar->width;
                demuxer->height = stream->codecpar->height;
                demuxer->codecId = stream->codecpar->codec_id;
                demuxer->colorRange = stream->codecpar->color_range;
                demuxer->colorSpace = stream->codecpar->color_space;
                
                if (stream->avg_frame_rate.den > 0) {
                    demuxer->frameRate = av_q2d(stream->avg_frame_rate);
                } else if (stream->r_frame_rate.den > 0) {
                    demuxer->frameRate = av_q2d(stream->r_frame_rate);
                }
                
                demuxer->timeBase = av_q2d(stream->time_base);
                
                if (demuxer->formatCtx->duration > 0) {
                    double duration = static_cast<double>(demuxer->formatCtx->duration) / AV_TIME_BASE;
                    demuxer->totalFrames = static_cast<int>(duration * demuxer->frameRate);
                } else if (stream->nb_frames > 0) {
                    demuxer->totalFrames = stream->nb_frames;
                } else {
                    demuxer->totalFrames = 100;
                }
            } else if (demuxer->alphaStreamIndex < 0) {
                demuxer->alphaStreamIndex = i;
                demuxer->alphaRange = stream->codecpar->color_range;
                demuxer->alphaSpace = stream->codecpar->color_space;
            }
        }
    }
    
    if (demuxer->colorStreamIndex < 0) {
        LOGE("[Buffered] No VP9/AV1 video stream found");
        return 0;
    }
    
    int streamCount = (demuxer->alphaStreamIndex >= 0) ? 2 : 1;
    LOGI("[Buffered] Demuxer created: %dx%d, %d frames, %.2f fps, %d streams, codec=%d",
         demuxer->width, demuxer->height, demuxer->totalFrames, demuxer->frameRate, streamCount, (int)demuxer->codecId);
    
    std::lock_guard<std::mutex> lock(demuxersMutex);
    jlong id = nextDemuxerId++;
    demuxers[id] = std::move(demuxer);
    
    return id;
}

} // anonymous namespace

JNIEXPORT jboolean JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeIsDualStream(
    JNIEnv* env,
    jclass clazz,
    jint fd,
    jlong offset,
    jlong length
) {
    AVFormatContext* formatCtx = nullptr;
    AVIOContext* io = nullptr;
    if (!openRangeInput(fd, offset, length, &formatCtx, &io)) {
        return JNI_FALSE;
    }
    return hasDualVideoStreams(formatCtx, io) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeIsDualStreamDownload(
    JNIEnv* env,
    jclass clazz,
    jstring path
) {
    AVFormatContext* formatCtx = nullptr;
    AVIOContext* io = nullptr;
    if (!openDownloadInput(stringFromJava(env, path), nullptr, &formatCtx, &io)) {
        return JNI_FALSE;
    }
    return hasDualVideoStreams(formatCtx, io) ? JNI_TRUE : JNI_FALSE;
}

// ============================================================================
// BufferedDualStreamPlayer JNI bindings (FBO ring buffer player)
// These reuse the same demuxer infrastructure
// ============================================================================

JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeCreateDemuxer(
    JNIEnv* env,
    jclass clazz,
    jint fd,
    jlong offset,
    jlong length
) {
    auto demuxer = std::make_shared<DualStreamDemuxer>();
    if (!openRangeInput(fd, offset, length, &demuxer->formatCtx, &demuxer->io)) {
        return 0;
    }
    return registerDemuxer(std::move(demuxer));
}

JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeCreateDemuxerFromDownload(
    JNIEnv* env,
    jclass clazz,
    jstring path
) {
    auto demuxer = std::make_shared<DualStreamDemuxer>();
    if (!openDownloadInput(stringFromJava(env, path), &demuxer->aborted, &demuxer->formatCtx, &demuxer->io)) {
        return 0;
    }
    return registerDemuxer(std::move(demuxer));
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeDestroyDemuxer(
    JNIEnv* env,
    jclass clazz,
    jlong demuxerId
) {
    std::shared_ptr<DualStreamDemuxer> demuxer;
    {
        std::lock_guard<std::mutex> lock(demuxersMutex);
        auto it = demuxers.find(demuxerId);
        if (it == demuxers.end()) return;
        demuxer = std::move(it->second);
        demuxers.erase(it);
    }
    demuxer->aborted = true;
    LOGI("[Buffered] Demuxer %lld destroyed", demuxerId);
}

JNIEXPORT jintArray JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeGetVideoInfo(
    JNIEnv* env,
    jclass clazz,
    jlong demuxerId
) {
    std::lock_guard<std::mutex> lock(demuxersMutex);
    auto it = demuxers.find(demuxerId);
    if (it == demuxers.end()) return nullptr;
    
    auto& demuxer = it->second;
    int streamCount = (demuxer->alphaStreamIndex >= 0) ? 2 : 1;
    
    // Return: [width, height, totalFrames, frameRate*1000, streamCount, codecId]
    jintArray result = env->NewIntArray(6);
    jint values[] = {
        demuxer->width,
        demuxer->height,
        demuxer->totalFrames,
        static_cast<int>(demuxer->frameRate * 1000),
        streamCount,
        static_cast<int>(demuxer->codecId)
    };
    env->SetIntArrayRegion(result, 0, 6, values);
    
    return result;
}

// [colour range, colour space, alpha range, alpha space] as FFmpeg's AVColorRange and AVColorSpace
JNIEXPORT jintArray JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeGetColorInfo(
    JNIEnv* env,
    jclass clazz,
    jlong demuxerId
) {
    std::shared_ptr<DualStreamDemuxer> demuxer = findDemuxer(demuxerId);
    if (!demuxer) return nullptr;
    jint values[] = {demuxer->colorRange, demuxer->colorSpace, demuxer->alphaRange, demuxer->alphaSpace};
    jintArray result = env->NewIntArray(4);
    env->SetIntArrayRegion(result, 0, 4, values);
    return result;
}

JNIEXPORT jobjectArray JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeReadPacket(
    JNIEnv* env,
    jclass clazz,
    jlong demuxerId
) {
    // Returns Object[] with: [Integer streamIndex, ByteBuffer data, Integer size, Long pts, Integer frameIndex]
    std::shared_ptr<DualStreamDemuxer> demuxer = findDemuxer(demuxerId);
    if (!demuxer) return nullptr;
    
    AVPacket* packet = av_packet_alloc();
    if (!packet) return nullptr;
    
    int ret;
    while ((ret = av_read_frame(demuxer->formatCtx, packet)) >= 0) {
        int streamIndex = packet->stream_index;
        
        if (streamIndex == demuxer->colorStreamIndex || 
            streamIndex == demuxer->alphaStreamIndex) {
            
            int mappedStreamIndex = (streamIndex == demuxer->colorStreamIndex) ? 0 : 1;
            
            int frameIndex;
            if (mappedStreamIndex == 0) {
                frameIndex = demuxer->colorFrameIndex++;
            } else {
                frameIndex = demuxer->alphaFrameIndex++;
            }
            
            long presentationTimeUs = 0;
            if (packet->pts != AV_NOPTS_VALUE) {
                AVStream* stream = demuxer->formatCtx->streams[streamIndex];
                presentationTimeUs = av_rescale_q(packet->pts, stream->time_base, 
                                                  AVRational{1, 1000000});
            }
            
            // Create Object[5] array
            jclass objectClass = env->FindClass("java/lang/Object");
            jobjectArray result = env->NewObjectArray(5, objectClass, nullptr);
            
            // [0] Integer streamIndex
            jclass integerClass = env->FindClass("java/lang/Integer");
            jmethodID intValueOf = env->GetStaticMethodID(integerClass, "valueOf", "(I)Ljava/lang/Integer;");
            env->SetObjectArrayElement(result, 0, env->CallStaticObjectMethod(integerClass, intValueOf, mappedStreamIndex));
            
            // [1] ByteBuffer data (direct buffer)
            jobject byteBuffer = env->NewDirectByteBuffer(packet->data, packet->size);
            // Make a copy since packet will be freed
            jclass byteBufferClass = env->FindClass("java/nio/ByteBuffer");
            jmethodID allocate = env->GetStaticMethodID(byteBufferClass, "allocate", "(I)Ljava/nio/ByteBuffer;");
            jobject copyBuffer = env->CallStaticObjectMethod(byteBufferClass, allocate, packet->size);
            jmethodID putMethod = env->GetMethodID(byteBufferClass, "put", "(Ljava/nio/ByteBuffer;)Ljava/nio/ByteBuffer;");
            env->CallObjectMethod(copyBuffer, putMethod, byteBuffer);
            jmethodID rewindMethod = env->GetMethodID(byteBufferClass, "rewind", "()Ljava/nio/Buffer;");
            env->CallObjectMethod(copyBuffer, rewindMethod);
            env->SetObjectArrayElement(result, 1, copyBuffer);
            
            // [2] Integer size
            env->SetObjectArrayElement(result, 2, env->CallStaticObjectMethod(integerClass, intValueOf, packet->size));
            
            // [3] Long pts
            jclass longClass = env->FindClass("java/lang/Long");
            jmethodID longValueOf = env->GetStaticMethodID(longClass, "valueOf", "(J)Ljava/lang/Long;");
            env->SetObjectArrayElement(result, 3, env->CallStaticObjectMethod(longClass, longValueOf, presentationTimeUs));
            
            // [4] Integer frameIndex
            env->SetObjectArrayElement(result, 4, env->CallStaticObjectMethod(integerClass, intValueOf, frameIndex));
            
            av_packet_unref(packet);
            av_packet_free(&packet);
            
            return result;
        }
        
        av_packet_unref(packet);
    }
    
    av_packet_free(&packet);
    return nullptr;
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeSeekToStart(
    JNIEnv* env,
    jclass clazz,
    jlong demuxerId
) {
    // Not under demuxersMutex: on a download the seek reads the Cues, which may still be arriving
    std::shared_ptr<DualStreamDemuxer> demuxer = findDemuxer(demuxerId);
    if (!demuxer) return;
    
    av_seek_frame(demuxer->formatCtx, demuxer->colorStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
    demuxer->colorFrameIndex = 0;
    demuxer->alphaFrameIndex = 0;
    
    LOGI("[Buffered] Demuxer %lld seeked to start", demuxerId);
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeSeekToTime(
    JNIEnv* env,
    jclass clazz,
    jlong demuxerId,
    jlong timeUs
) {
    // To the keyframe at or before timeUs, in the same timebase as the packet PTS handed out.
    // Not under demuxersMutex, for the same reason as nativeSeekToStart.
    std::shared_ptr<DualStreamDemuxer> demuxer = findDemuxer(demuxerId);
    if (!demuxer) return;
    
    AVStream* stream = demuxer->formatCtx->streams[demuxer->colorStreamIndex];
    int64_t ts = av_rescale_q(timeUs, AVRational{1, 1000000}, stream->time_base);
    av_seek_frame(demuxer->formatCtx, demuxer->colorStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
}

} // extern "C"
