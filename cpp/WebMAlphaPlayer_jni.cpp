/**
 * WebMAlphaPlayer JNI Bridge
 * Connects Java/Kotlin to C++ WebMAlphaPlayer core
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "WebMAlphaPlayer.h"
#include "AlphaDemuxer.h"
#include "ByteSource.h"
#include "RangeReader.h"
#include "VideoDecoder.h"
#include <pthread.h>
#include <algorithm>
#include <atomic>
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
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

//==============================================================================
// Range readers over a Java AlphaVideoDataSource (SourceReader.Upstream)
//==============================================================================

pthread_key_t detachKey;
pthread_once_t detachKeyOnce = PTHREAD_ONCE_INIT;

void detachThread(void* vm) {
    static_cast<JavaVM*>(vm)->DetachCurrentThread();
}

// This thread's env. A native thread is attached until it exits, as a reader's fetch thread calls
// into Java for as long as it runs.
JNIEnv* threadEnv(JavaVM* vm) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_once(&detachKeyOnce, [] { pthread_key_create(&detachKey, detachThread); });
    pthread_setspecific(detachKey, vm);
    return env;
}

// Clears a pending Java exception; true if there was one.
bool clearException(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

class JavaUpstream : public avsource::Upstream {
public:
    static std::unique_ptr<JavaUpstream> create(JNIEnv* env, jobject upstream) {
        auto result = std::unique_ptr<JavaUpstream>(new JavaUpstream());
        env->GetJavaVM(&result->vm_);
        jclass cls = env->GetObjectClass(upstream);
        result->open_ = env->GetMethodID(cls, "open", "(J)J");
        result->read_ = env->GetMethodID(cls, "read", "([BI)I");
        result->close_ = env->GetMethodID(cls, "close", "()V");
        result->cancel_ = env->GetMethodID(cls, "cancel", "()V");
        result->etag_ = env->GetFieldID(cls, "etag", "Ljava/lang/String;");
        result->lastModified_ = env->GetFieldID(cls, "lastModified", "Ljava/lang/String;");
        result->error_ = env->GetFieldID(cls, "error", "Ljava/lang/String;");
        env->DeleteLocalRef(cls);
        jbyteArray buffer = env->NewByteArray(kBufferBytes);
        if (clearException(env) || !buffer || !result->open_ || !result->read_ || !result->close_ ||
            !result->cancel_ || !result->etag_ || !result->lastModified_ || !result->error_) {
            return nullptr;
        }
        result->object_ = env->NewGlobalRef(upstream);
        result->buffer_ = static_cast<jbyteArray>(env->NewGlobalRef(buffer));
        env->DeleteLocalRef(buffer);
        return result;
    }

    ~JavaUpstream() override {
        JNIEnv* env = threadEnv(vm_);
        if (!env) return;
        if (object_) env->DeleteGlobalRef(object_);
        if (buffer_) env->DeleteGlobalRef(buffer_);
    }

    bool open(int64_t position, avsource::SourceInfo& info, std::string& error) override {
        JNIEnv* env = threadEnv(vm_);
        if (!env) {
            error = "Cannot attach the reader's thread to Java";
            return false;
        }
        jlong size = env->CallLongMethod(object_, open_, static_cast<jlong>(position));
        if (clearException(env)) {
            error = "The data source threw";
            return false;
        }
        if (size == -2) {
            error = stringField(env, error_);
            return false;
        }
        info.size = size >= 0 ? size : -1;
        info.etag = stringField(env, etag_);
        info.lastModified = stringField(env, lastModified_);
        return true;
    }

    int64_t read(uint8_t* out, int64_t size, std::string& error) override {
        JNIEnv* env = threadEnv(vm_);
        if (!env) {
            error = "Cannot attach the reader's thread to Java";
            return -1;
        }
        jint n = env->CallIntMethod(object_, read_, buffer_, static_cast<jint>(std::min<int64_t>(size, kBufferBytes)));
        if (clearException(env)) {
            error = "The data source threw";
            return -1;
        }
        if (n < 0) {
            error = stringField(env, error_);
            return -1;
        }
        if (n > 0) env->GetByteArrayRegion(buffer_, 0, n, reinterpret_cast<jbyte*>(out));
        return n;
    }

    void close() override { call(close_); }
    void cancel() override { call(cancel_); }

private:
    static constexpr jint kBufferBytes = 64 * 1024;

    JavaUpstream() = default;

    void call(jmethodID method) {
        JNIEnv* env = threadEnv(vm_);
        if (!env) return;
        env->CallVoidMethod(object_, method);
        clearException(env);
    }

    std::string stringField(JNIEnv* env, jfieldID field) {
        auto value = static_cast<jstring>(env->GetObjectField(object_, field));
        std::string result = stringFromJava(env, value);
        if (value) env->DeleteLocalRef(value);
        return result;
    }

    JavaVM* vm_ = nullptr;
    jobject object_ = nullptr;
    jbyteArray buffer_ = nullptr;
    jmethodID open_ = nullptr, read_ = nullptr, close_ = nullptr, cancel_ = nullptr;
    jfieldID etag_ = nullptr, lastModified_ = nullptr, error_ = nullptr;
};

// A SourceReader reference: a shared_ptr of its own, so each holder frees only its own.
using ReaderRef = std::shared_ptr<avsource::RangeReader>;

const ReaderRef& readerOf(jlong reference) {
    return *reinterpret_cast<ReaderRef*>(reference);
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
    webm::PlaybackMode mode = webm::PlaybackMode::Stream;
    if (modeStr != nullptr) {
        const char* modeChars = env->GetStringUTFChars(modeStr, nullptr);
        if (modeChars) {
            std::string modeString(modeChars);
            if (modeString == "preload") {
                mode = webm::PlaybackMode::Preload;
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
        refs.onError = env->GetMethodID(listenerClass, "onError", "(Ljava/lang/String;Ljava/lang/String;)V");
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
    
    std::weak_ptr<webm::WebMAlphaPlayer> weakPlayer = player;
    player->setEventCallback([playerId, weakPlayer](const std::string& event, const std::string& data) {
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
            auto erring = weakPlayer.lock();
            const std::string code = erring ? erring->getLastErrorCode() : std::string();
            jstring errStr = env->NewStringUTF(data.c_str());
            jstring codeStr = code.empty() ? nullptr : env->NewStringUTF(code.c_str());
            env->CallVoidMethod(it->second.listener, it->second.onError, errStr, codeStr);
            env->DeleteLocalRef(errStr);
            if (codeStr) env->DeleteLocalRef(codeStr);
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
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeLoadFromReader(
    JNIEnv* env,
    jclass clazz,
    jlong playerId,
    jlong reader
) {
    {
        std::lock_guard<std::mutex> dlock(destroyedMutex);
        if (destroyedPlayers.count(playerId)) return JNI_FALSE;
    }
    
    {
        std::lock_guard<std::mutex> slock(stoppedMutex);
        stoppedPlayers.erase(playerId);
    }
    
    std::shared_ptr<webm::WebMAlphaPlayer> player = findPlayer(playerId);
    if (!player) return JNI_FALSE;
    return player->loadFromReader(readerOf(reader)) ? JNI_TRUE : JNI_FALSE;
}

//==============================================================================
// Range readers (SourceReader)
//==============================================================================

JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_SourceReader_nativeOpen(
    JNIEnv* env,
    jclass clazz,
    jobject upstream,
    jint storage,
    jstring storeDir,
    jstring key,
    jlong maxStorageBytes,
    jobjectArray errorOut
) {
    // SourceReader.Storage ordinals
    enum { kDisk = 0, kMemory = 1 };
    std::string error;
    std::shared_ptr<avsource::RangeReader> reader;
    std::unique_ptr<JavaUpstream> javaUpstream = JavaUpstream::create(env, upstream);
    if (javaUpstream) {
        std::shared_ptr<avsource::Store> store;
        if (storage == kDisk) {
            store = avsource::Store::disk(stringFromJava(env, storeDir), maxStorageBytes);
        } else if (storage == kMemory) {
            store = avsource::Store::memory(stringFromJava(env, storeDir), maxStorageBytes);
        }
        reader = avsource::RangeReader::open(std::move(javaUpstream), std::move(store), stringFromJava(env, key),
                                             avsource::ReaderOptions(), error);
    } else {
        error = "Cannot call the data source";
    }
    if (!reader) {
        jstring message = env->NewStringUTF(error.c_str());
        env->SetObjectArrayElement(errorOut, 0, message);
        env->DeleteLocalRef(message);
        return 0;
    }
    return reinterpret_cast<jlong>(new ReaderRef(std::move(reader)));
}

JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_SourceReader_nativeRetain(JNIEnv* env, jclass clazz, jlong reference) {
    return reinterpret_cast<jlong>(new ReaderRef(readerOf(reference)));
}

JNIEXPORT void JNICALL
Java_dev_mediacache_alphavideo_SourceReader_nativeRelease(JNIEnv* env, jclass clazz, jlong reference) {
    delete reinterpret_cast<ReaderRef*>(reference);
}

JNIEXPORT jstring JNICALL
Java_dev_mediacache_alphavideo_SourceReader_nativeFetchAll(JNIEnv* env, jclass clazz, jlong reference) {
    const ReaderRef& reader = readerOf(reference);
    if (reader->fetchAll()) return nullptr;
    std::string error = reader->lastError();
    return env->NewStringUTF(error.empty() ? "Reading the video failed" : error.c_str());
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

JNIEXPORT jfloatArray JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeYuvToRgbFor(
    JNIEnv* env, jclass clazz, jint colorRange, jint colorSpace, jint width, jint height) {
    webm::YuvToRgb m = webm::yuvToRgbFor(colorRange, colorSpace, width, height);
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

JNIEXPORT jstring JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetDecoderName(JNIEnv* env, jclass clazz, jlong playerId) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(playersMutex);
        auto it = players.find(playerId);
        if (it == players.end()) return nullptr;
        name = it->second->getDecoderName();
    }
    return env->NewStringUTF(name.c_str());
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
// Decoder library versions
//==============================================================================

JNIEXPORT jstring JNICALL
Java_dev_mediacache_alphavideo_WebMAlphaPlayerNative_nativeGetLibraryVersions(JNIEnv* env, jclass clazz) {
    std::string versions = webm::WebMAlphaPlayer::getLibraryVersions();
    return env->NewStringUTF(versions.c_str());
}

//==============================================================================
// Software decoders (codec modules)
//==============================================================================

// Null if registered, else why not
JNIEXPORT jstring JNICALL
Java_dev_mediacache_alphavideo_AlphaVideoCodecs_nativeRegister(JNIEnv* env, jclass clazz, jlong pluginList) {
    if (!pluginList) return env->NewStringUTF("A codec module gave no plugin list");
    std::string error;
    if (webm::registerDecoderPlugins(reinterpret_cast<AlphaVideoDecoderPluginList>(pluginList), &error)) {
        return nullptr;
    }
    return env->NewStringUTF(error.c_str());
}

JNIEXPORT jobjectArray JNICALL
Java_dev_mediacache_alphavideo_AlphaVideoCodecs_nativeSoftwareDecoders(JNIEnv* env, jclass clazz) {
    const std::vector<std::string> decoders = webm::registeredDecoders();
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(decoders.size()),
                                              env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < decoders.size(); i++) {
        jstring decoder = env->NewStringUTF(decoders[i].c_str());
        env->SetObjectArrayElement(result, static_cast<jsize>(i), decoder);
        env->DeleteLocalRef(decoder);
    }
    return result;
}

//==============================================================================
// Dual-Stream Demuxer for MediaCodec Hardware Decoding
// Extracts raw VP8/VP9/AV1 packets from dual-stream WebM files
//==============================================================================

namespace {

// A demuxer, which owns the source it reads. The demuxer is freed before the abort flag its
// download source polls.
struct DualStreamDemuxer {
    // Set on destroy so a read waiting on a download returns
    std::atomic<bool> aborted{false};
    std::unique_ptr<webm::AlphaDemuxer> demuxer;
};

jintArray intArray(JNIEnv* env, const std::vector<jint>& values) {
    jintArray result = env->NewIntArray(static_cast<jsize>(values.size()));
    if (result) env->SetIntArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    return result;
}

// The colour and alpha bitstreams of source, for MediaCodec: each one's codec (0 VP9, 1 AV1, 2 VP8) and
// profile, in pairs (AlphaDemuxer::colorAlphaTracks). Null unless there are two.
jintArray colorAlphaTracks(JNIEnv* env, std::unique_ptr<webm::ByteSource> source) {
    std::vector<int> tracks = webm::AlphaDemuxer::colorAlphaTracks(std::move(source));
    if (tracks.size() < 4) return nullptr;
    return intArray(env, std::vector<jint>(tracks.begin(), tracks.end()));
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

// Opens the demuxer over source and registers it. Returns its id, or 0.
jlong registerDemuxer(std::shared_ptr<DualStreamDemuxer> demuxer, std::unique_ptr<webm::ByteSource> source) {
    if (!source) return 0;
    demuxer->demuxer = webm::AlphaDemuxer::open(std::move(source));
    if (!demuxer->demuxer) {
        LOGE("[Buffered] No VP8/VP9/AV1 video stream found, or the file can't be opened");
        return 0;
    }
    const webm::AlphaDemuxer::Info& info = demuxer->demuxer->info();
    LOGI("[Buffered] Demuxer created: %dx%d, %d frames, %.2f fps, %d streams, codec=%d",
         info.width, info.height, info.totalFrames, info.frameRate, info.hasAlpha ? 2 : 1, (int)info.color.codec);

    std::lock_guard<std::mutex> lock(demuxersMutex);
    jlong id = nextDemuxerId++;
    demuxers[id] = std::move(demuxer);
    return id;
}

} // anonymous namespace

JNIEXPORT jintArray JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeColorAlphaTracks(
    JNIEnv* env,
    jclass clazz,
    jint fd,
    jlong offset,
    jlong length
) {
    std::unique_ptr<webm::ByteSource> source = webm::openFileRange(fd, offset, length);
    if (!source) {
        LOGE("Failed to open fd %d range (offset %lld, length %lld)", fd, (long long)offset, (long long)length);
        return nullptr;
    }
    return colorAlphaTracks(env, std::move(source));
}

JNIEXPORT jintArray JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeColorAlphaTracksReader(
    JNIEnv* env,
    jclass clazz,
    jlong reader
) {
    return colorAlphaTracks(env, webm::openReader(readerOf(reader), nullptr));
}

// ============================================================================
// BufferedDualStreamPlayer JNI bindings (FBO ring buffer player)
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
    std::unique_ptr<webm::ByteSource> source = webm::openFileRange(fd, offset, length);
    return registerDemuxer(std::move(demuxer), std::move(source));
}

JNIEXPORT jlong JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeCreateDemuxerFromReader(
    JNIEnv* env,
    jclass clazz,
    jlong reader
) {
    auto demuxer = std::make_shared<DualStreamDemuxer>();
    std::unique_ptr<webm::ByteSource> source = webm::openReader(readerOf(reader), &demuxer->aborted);
    return registerDemuxer(std::move(demuxer), std::move(source));
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
    std::shared_ptr<DualStreamDemuxer> demuxer = findDemuxer(demuxerId);
    if (!demuxer) return nullptr;
    const webm::AlphaDemuxer::Info& info = demuxer->demuxer->info();
    // [width, height, totalFrames, frameRate*1000, streamCount, codec (0 VP9, 1 AV1, 2 VP8)]
    return intArray(env, {
        info.width,
        info.height,
        info.totalFrames,
        static_cast<jint>(info.frameRate * 1000),
        info.hasAlpha ? 2 : 1,
        static_cast<jint>(info.color.codec),
    });
}

// [colour range, colour space, alpha range, alpha space] as H.273 numbers (YuvToRgb.h)
JNIEXPORT jintArray JNICALL
Java_dev_mediacache_alphavideo_BufferedDualStreamPlayer_nativeGetColorInfo(
    JNIEnv* env,
    jclass clazz,
    jlong demuxerId
) {
    std::shared_ptr<DualStreamDemuxer> demuxer = findDemuxer(demuxerId);
    if (!demuxer) return nullptr;
    const webm::AlphaDemuxer::Info& info = demuxer->demuxer->info();
    return intArray(env, {info.color.colorRange, info.color.colorSpace, info.alpha.colorRange, info.alpha.colorSpace});
}

// [Integer streamIndex, ByteBuffer data (a copy), Integer size, Long pts, Integer frameIndex]
static jobjectArray packetArray(JNIEnv* env, int streamIndex, const uint8_t* data, int size,
                                long presentationTimeUs, int frameIndex) {
    jclass objectClass = env->FindClass("java/lang/Object");
    jobjectArray result = env->NewObjectArray(5, objectClass, nullptr);
    
    // [0] Integer streamIndex
    jclass integerClass = env->FindClass("java/lang/Integer");
    jmethodID intValueOf = env->GetStaticMethodID(integerClass, "valueOf", "(I)Ljava/lang/Integer;");
    env->SetObjectArrayElement(result, 0, env->CallStaticObjectMethod(integerClass, intValueOf, streamIndex));
    
    // [1] ByteBuffer data, copied since the packet will be freed
    jclass byteBufferClass = env->FindClass("java/nio/ByteBuffer");
    jmethodID allocate = env->GetStaticMethodID(byteBufferClass, "allocate", "(I)Ljava/nio/ByteBuffer;");
    jobject copyBuffer = env->CallStaticObjectMethod(byteBufferClass, allocate, size);
    if (size > 0) {
        jobject byteBuffer = env->NewDirectByteBuffer(const_cast<uint8_t*>(data), size);
        jmethodID putMethod = env->GetMethodID(byteBufferClass, "put", "(Ljava/nio/ByteBuffer;)Ljava/nio/ByteBuffer;");
        env->CallObjectMethod(copyBuffer, putMethod, byteBuffer);
        jmethodID rewindMethod = env->GetMethodID(byteBufferClass, "rewind", "()Ljava/nio/Buffer;");
        env->CallObjectMethod(copyBuffer, rewindMethod);
    }
    env->SetObjectArrayElement(result, 1, copyBuffer);
    
    // [2] Integer size
    env->SetObjectArrayElement(result, 2, env->CallStaticObjectMethod(integerClass, intValueOf, size));
    
    // [3] Long pts
    jclass longClass = env->FindClass("java/lang/Long");
    jmethodID longValueOf = env->GetStaticMethodID(longClass, "valueOf", "(J)Ljava/lang/Long;");
    env->SetObjectArrayElement(result, 3, env->CallStaticObjectMethod(longClass, longValueOf, presentationTimeUs));
    
    // [4] Integer frameIndex
    env->SetObjectArrayElement(result, 4, env->CallStaticObjectMethod(integerClass, intValueOf, frameIndex));
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
    webm::AlphaDemuxer::Packet packet;
    if (!demuxer->demuxer->readPacket(&packet)) return nullptr;
    return packetArray(env, packet.stream, packet.data, packet.size, static_cast<long>(packet.ptsUs), packet.frameIndex);
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
    demuxer->demuxer->seekToStart();
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
    demuxer->demuxer->seekToTimeUs(timeUs);
}

} // extern "C"
