/**
 * WebMAlphaPlayer JNI Bridge
 * Connects Java/Kotlin to C++ WebMAlphaPlayer core
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "WebMAlphaPlayer.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <string>

#define LOG_TAG "WebMAlphaPlayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Store player instances by ID
// Using shared_ptr to allow safe reference during async operations
std::unordered_map<jlong, std::shared_ptr<webm::WebMAlphaPlayer>> players;
std::mutex playersMutex;
jlong nextPlayerId = 1;

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

} // anonymous namespace

extern "C" {

//==============================================================================
// Player Lifecycle
//==============================================================================

JNIEXPORT jlong JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeCreate(
    JNIEnv* env,
    jclass clazz,
    jint decodeThreads,
    jint cacheSize,
    jboolean preloadAllFrames,
    jboolean loopPlayback,
    jdouble playbackSpeed,
    jboolean stream,
    jint streamCacheSize,
    jint streamPrefetchAhead
) {
    auto player = std::make_shared<webm::WebMAlphaPlayer>();
    
    webm::PlayerConfig config;
    config.decodeThreads = decodeThreads;
    config.cacheSize = cacheSize;
    config.preloadAllFrames = preloadAllFrames;
    config.loopPlayback = loopPlayback;
    config.playbackSpeed = playbackSpeed;
    config.stream = stream;
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
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeDestroy(
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
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeSetListener(
    JNIEnv* env,
    jclass clazz,
    jlong playerId,
    jobject listener
) {
    std::lock_guard<std::mutex> plock(playersMutex);
    auto playerIt = players.find(playerId);
    if (playerIt == players.end()) return;
    
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
    auto& player = playerIt->second;
    
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
            // Parse "X frames in Yms"
            int frames = std::stoi(data.substr(0, data.find(' ')));
            double ms = std::stod(data.substr(data.find("in ") + 3));
            env->CallVoidMethod(it->second.listener, it->second.onPreloadComplete, frames, ms);
        }
    });
    
    LOGI("Set listener for player %lld", playerId);
}

//==============================================================================
// Loading
//==============================================================================

JNIEXPORT jboolean JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeLoadFromFile(
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
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeLoadFromBytes(
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

JNIEXPORT jboolean JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeLoadFromUrl(
    JNIEnv* env,
    jclass clazz,
    jlong playerId,
    jstring url
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
    
    const char* urlStr = env->GetStringUTFChars(url, nullptr);
    std::string urlCopy(urlStr);  // Copy before releasing
    env->ReleaseStringUTFChars(url, urlStr);
    
    // Check again if destroyed before expensive operation
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) {
            LOGI("LoadFromUrl aborted - player %lld destroyed", playerId);
            return JNI_FALSE;
        }
    }
    
    bool result = player->loadFromUrl(urlCopy);
    
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeUnload(
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
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativePlay(JNIEnv* env, jclass clazz, jlong playerId) {
    // Remove from stopped set when playing resumes
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(playerId);
    }
    
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    if (it != players.end()) it->second->play();
}

JNIEXPORT void JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativePause(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    if (it != players.end()) it->second->pause();
}

JNIEXPORT void JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeStop(JNIEnv* env, jclass clazz, jlong playerId) {
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
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeSeekToFrame(
    JNIEnv* env, jclass clazz, jlong playerId, jint frameIndex
) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    if (it != players.end()) it->second->seekToFrame(frameIndex);
}

JNIEXPORT void JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeSeekToTime(
    JNIEnv* env, jclass clazz, jlong playerId, jdouble seconds
) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    if (it != players.end()) it->second->seekTo(seconds);
}

//==============================================================================
// State Queries
//==============================================================================

JNIEXPORT jboolean JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeIsLoaded(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end() && it->second->isLoaded()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeIsPlaying(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end() && it->second->isPlaying()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetCurrentFrame(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getCurrentFrame() : 0;
}

JNIEXPORT jint JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetTotalFrames(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getTotalFrames() : 0;
}

JNIEXPORT jint JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetWidth(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getWidth() : 0;
}

JNIEXPORT jint JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetHeight(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getHeight() : 0;
}

JNIEXPORT jdouble JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetDuration(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getDuration() : 0.0;
}

JNIEXPORT jdouble JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetFrameRate(JNIEnv* env, jclass clazz, jlong playerId) {
    std::lock_guard<std::mutex> lock(playersMutex);
    auto it = players.find(playerId);
    return (it != players.end()) ? it->second->getFrameRate() : 0.0;
}

//==============================================================================
// Direct Frame Access
//==============================================================================

JNIEXPORT jobject JNICALL
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetFrame(
    JNIEnv* env, jclass clazz, jlong playerId, jint frameIndex
) {
    // Check if destroyed first
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) return nullptr;
    }
    
    // Get frame while holding lock - keep shared_ptr alive
    std::shared_ptr<webm::YUVAFrame> frame;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return nullptr;
        
        frame = it->second->getFrame(frameIndex);
        if (!frame) return nullptr;
    }
    
    // Now we have a shared_ptr keeping the frame alive.
    // CRITICAL: Create Java-owned byte arrays and copy data!
    // The C++ frame could be freed when the cache is cleared or player destroyed.
    
    // Create Java byte arrays and copy data
    jbyteArray yArray = env->NewByteArray(frame->ySize);
    jbyteArray uArray = env->NewByteArray(frame->uSize);
    jbyteArray vArray = env->NewByteArray(frame->vSize);
    jbyteArray aArray = env->NewByteArray(frame->aSize);
    
    if (!yArray || !uArray || !vArray || !aArray) {
        LOGI("Failed to allocate byte arrays for frame %d", frameIndex);
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
    jclass frameClass = env->FindClass("com/danielkpl2/webmalphaplayer/WebMFrame");
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
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetStats(JNIEnv* env, jclass clazz, jlong playerId) {
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
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeResetStats(JNIEnv* env, jclass clazz, jlong playerId) {
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
Java_com_danielkpl2_webmalphaplayer_WebMAlphaPlayerNative_nativeGetFFmpegVersion(JNIEnv* env, jclass clazz) {
    std::string version = webm::WebMAlphaPlayer::getFFmpegVersion();
    return env->NewStringUTF(version.c_str());
}

} // extern "C"
