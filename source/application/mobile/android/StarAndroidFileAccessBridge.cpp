#include "StarAndroidFileAccessBridge.hpp"

#ifdef STAR_SYSTEM_ANDROID
#include "SDL3/SDL_system.h"
#include <algorithm>
#include <jni.h>
#endif

namespace Star {

#ifdef STAR_SYSTEM_ANDROID
namespace {

JNIEnv* jniEnv() {
  return reinterpret_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
}

template <typename T>
class JniLocalRef {
public:
  JniLocalRef(JNIEnv* env = nullptr, T ref = nullptr)
    : m_env(env), m_ref(ref) {}

  JniLocalRef(JniLocalRef const&) = delete;
  JniLocalRef& operator=(JniLocalRef const&) = delete;

  JniLocalRef(JniLocalRef&& rhs) noexcept
    : m_env(rhs.m_env), m_ref(rhs.m_ref) {
    rhs.m_env = nullptr;
    rhs.m_ref = nullptr;
  }

  ~JniLocalRef() {
    reset();
  }

  explicit operator bool() const {
    return m_ref != nullptr;
  }

  T get() const {
    return m_ref;
  }

  void reset(T ref = nullptr) {
    if (m_env && m_ref)
      m_env->DeleteLocalRef(m_ref);
    m_ref = ref;
  }

private:
  JNIEnv* m_env;
  T m_ref;
};

JniLocalRef<jclass> mainActivityClass(JNIEnv* env) {
  jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
  if (!activity)
    return {};

  JniLocalRef<jobject> activityRef(env, activity);
  return JniLocalRef<jclass>(env, env->GetObjectClass(activity));
}

String toStarString(JNIEnv* env, jstring value) {
  if (!value)
    return {};
  char const* utf = env->GetStringUTFChars(value, nullptr);
  String out = utf ? utf : "";
  if (utf)
    env->ReleaseStringUTFChars(value, utf);
  return out;
}

StringList callJavaStringArrayMethod(char const* methodName, String const& argValue) {
  StringList out;

  JNIEnv* env = jniEnv();
  if (!env)
    return out;

  auto cls = mainActivityClass(env);
  if (!cls)
    return out;

  jmethodID method = env->GetStaticMethodID(cls.get(), methodName, "(Ljava/lang/String;)[Ljava/lang/String;");
  if (!method)
    return out;

  jstring arg = env->NewStringUTF(argValue.utf8Ptr());
  jobjectArray result = (jobjectArray)env->CallStaticObjectMethod(cls.get(), method, arg);
  env->DeleteLocalRef(arg);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return out;
  }

  if (!result)
    return out;

  jsize count = env->GetArrayLength(result);
  for (jsize i = 0; i < count; ++i) {
    jstring item = (jstring)env->GetObjectArrayElement(result, i);
    String value = toStarString(env, item);
    if (!value.empty())
      out.append(value);
    if (item)
      env->DeleteLocalRef(item);
  }

  env->DeleteLocalRef(result);
  return out;
}

}
#endif

Maybe<String> AndroidFileAccessBridge::resolveStorageRoot(String const& fallbackStorageRootDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return {};

  auto cls = mainActivityClass(env);
  if (!cls)
    return {};

  jmethodID method = env->GetStaticMethodID(cls.get(), "resolveStorageRoot", "(Ljava/lang/String;)Ljava/lang/String;");
  if (!method)
    return {};

  jstring arg = env->NewStringUTF(fallbackStorageRootDirectory.utf8Ptr());
  jstring result = (jstring)env->CallStaticObjectMethod(cls.get(), method, arg);
  env->DeleteLocalRef(arg);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return {};
  }

  String value = toStarString(env, result);
  if (result)
    env->DeleteLocalRef(result);
  if (value.empty())
    return {};
  return value;
#else
  (void)fallbackStorageRootDirectory;
  return {};
#endif
}

Maybe<String> AndroidFileAccessBridge::syncBundledAssets(String const& targetRootDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return {};

  auto cls = mainActivityClass(env);
  if (!cls)
    return {};

  jmethodID method = env->GetStaticMethodID(cls.get(), "syncBundledAssets", "(Ljava/lang/String;)Ljava/lang/String;");
  if (!method)
    return {};

  jstring arg = env->NewStringUTF(targetRootDirectory.utf8Ptr());
  jstring result = (jstring)env->CallStaticObjectMethod(cls.get(), method, arg);
  env->DeleteLocalRef(arg);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return {};
  }

  String value = toStarString(env, result);
  if (result)
    env->DeleteLocalRef(result);
  if (value.empty())
    return {};
  return value;
#else
  (void)targetRootDirectory;
  return {};
#endif
}

Maybe<String> AndroidFileAccessBridge::pickAndImportPackedPak(String const& targetPath) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return {};

  auto cls = mainActivityClass(env);
  if (!cls)
    return {};

  jmethodID method = env->GetStaticMethodID(cls.get(), "pickPackedPakAndImport", "(Ljava/lang/String;)Ljava/lang/String;");
  if (!method)
    return {};

  jstring arg = env->NewStringUTF(targetPath.utf8Ptr());
  jstring result = (jstring)env->CallStaticObjectMethod(cls.get(), method, arg);
  env->DeleteLocalRef(arg);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return {};
  }

  String value = toStarString(env, result);
  if (result)
    env->DeleteLocalRef(result);
  if (value.empty())
    return {};
  return value;
#else
  (void)targetPath;
  return {};
#endif
}

Maybe<String> AndroidFileAccessBridge::resolveModsDirectory(String const& fallbackModsDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return {};

  auto cls = mainActivityClass(env);
  if (!cls)
    return {};

  jmethodID method = env->GetStaticMethodID(cls.get(), "resolveModsDirectory", "(Ljava/lang/String;)Ljava/lang/String;");
  if (!method)
    return {};

  jstring arg = env->NewStringUTF(fallbackModsDirectory.utf8Ptr());
  jstring result = (jstring)env->CallStaticObjectMethod(cls.get(), method, arg);
  env->DeleteLocalRef(arg);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return {};
  }

  String value = toStarString(env, result);
  if (result)
    env->DeleteLocalRef(result);
  if (value.empty())
    return {};
  return value;
#else
  (void)fallbackModsDirectory;
  return {};
#endif
}

StringList AndroidFileAccessBridge::importModPakFiles(String const& modsDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  return callJavaStringArrayMethod("importModPak", modsDirectory);
#else
  (void)modsDirectory;
  return {};
#endif
}

StringList AndroidFileAccessBridge::importSingleModFolder(String const& modsDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  return callJavaStringArrayMethod("importSingleModFolder", modsDirectory);
#else
  (void)modsDirectory;
  return {};
#endif
}

StringList AndroidFileAccessBridge::importModsDirectory(String const& modsDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  return callJavaStringArrayMethod("importModsFolder", modsDirectory);
#else
  (void)modsDirectory;
  return {};
#endif
}

bool AndroidFileAccessBridge::openModsDirectory(String const& modsDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return false;

  auto cls = mainActivityClass(env);
  if (!cls)
    return false;

  jmethodID method = env->GetStaticMethodID(cls.get(), "openModsDirectory", "(Ljava/lang/String;)Z");
  if (!method)
    return false;

  jstring arg = env->NewStringUTF(modsDirectory.utf8Ptr());
  jboolean result = env->CallStaticBooleanMethod(cls.get(), method, arg);
  env->DeleteLocalRef(arg);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }

  return result == JNI_TRUE;
#else
  (void)modsDirectory;
  return false;
#endif
}

bool AndroidFileAccessBridge::exportDiagnostics(String const& storageRootDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return false;

  auto cls = mainActivityClass(env);
  if (!cls)
    return false;

  jmethodID method = env->GetStaticMethodID(cls.get(), "exportDiagnostics", "(Ljava/lang/String;)Z");
  if (!method)
    return false;

  jstring arg = env->NewStringUTF(storageRootDirectory.utf8Ptr());
  jboolean result = env->CallStaticBooleanMethod(cls.get(), method, arg);
  env->DeleteLocalRef(arg);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }

  return result == JNI_TRUE;
#else
  (void)storageRootDirectory;
  return false;
#endif
}

void AndroidFileAccessBridge::showToast(String const& message) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return;

  auto cls = mainActivityClass(env);
  if (!cls)
    return;

  jmethodID method = env->GetStaticMethodID(cls.get(), "showToast", "(Ljava/lang/String;)V");
  if (!method)
    return;

  jstring arg = env->NewStringUTF(message.utf8Ptr());
  env->CallStaticVoidMethod(cls.get(), method, arg);
  env->DeleteLocalRef(arg);
  if (env->ExceptionCheck())
    env->ExceptionClear();
#else
  (void)message;
#endif
}

void AndroidFileAccessBridge::showDialog(String const& title, String const& message) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return;

  auto cls = mainActivityClass(env);
  if (!cls)
    return;

  jmethodID method = env->GetStaticMethodID(cls.get(), "showDialog", "(Ljava/lang/String;Ljava/lang/String;)V");
  if (!method)
    return;

  jstring jTitle = env->NewStringUTF(title.utf8Ptr());
  jstring jMessage = env->NewStringUTF(message.utf8Ptr());
  env->CallStaticVoidMethod(cls.get(), method, jTitle, jMessage);
  env->DeleteLocalRef(jTitle);
  env->DeleteLocalRef(jMessage);
  if (env->ExceptionCheck())
    env->ExceptionClear();
#else
  (void)title;
  (void)message;
#endif
}

bool AndroidFileAccessBridge::openAppSettings() {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return false;

  auto cls = mainActivityClass(env);
  if (!cls)
    return false;

  jmethodID method = env->GetStaticMethodID(cls.get(), "openAppSettings", "()Z");
  if (!method)
    return false;

  jboolean result = env->CallStaticBooleanMethod(cls.get(), method);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  return result == JNI_TRUE;
#else
  return false;
#endif
}

void AndroidFileAccessBridge::getSafeAreaInsets(unsigned* top, unsigned* left, unsigned* bottom, unsigned* right) {
  if (top)
    *top = 0;
  if (left)
    *left = 0;
  if (bottom)
    *bottom = 0;
  if (right)
    *right = 0;

#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return;

  auto cls = mainActivityClass(env);
  if (!cls)
    return;

  jmethodID method = env->GetStaticMethodID(cls.get(), "getSafeAreaInsets", "()[I");
  if (!method)
    return;

  jintArray result = (jintArray)env->CallStaticObjectMethod(cls.get(), method);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return;
  }

  if (!result)
    return;

  jint values[4] = {0, 0, 0, 0};
  jsize count = env->GetArrayLength(result);
  env->GetIntArrayRegion(result, 0, std::min<jsize>(count, 4), values);
  env->DeleteLocalRef(result);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return;
  }

  if (top)
    *top = (unsigned)std::max(0, values[0]);
  if (left)
    *left = (unsigned)std::max(0, values[1]);
  if (bottom)
    *bottom = (unsigned)std::max(0, values[2]);
  if (right)
    *right = (unsigned)std::max(0, values[3]);
#else
  (void)top;
  (void)left;
  (void)bottom;
  (void)right;
#endif
}

}
