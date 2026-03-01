#include "StarAndroidFileAccessBridge.hpp"

#ifdef STAR_SYSTEM_ANDROID
#include "SDL3/SDL_system.h"
#include <jni.h>
#endif

namespace Star {

#ifdef STAR_SYSTEM_ANDROID
namespace {

JNIEnv* jniEnv() {
  return reinterpret_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
}

jclass mainActivityClass(JNIEnv* env) {
  jobject activity = reinterpret_cast<jobject>(SDL_GetAndroidActivity());
  if (!activity)
    return nullptr;
  return env->GetObjectClass(activity);
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

  jclass cls = mainActivityClass(env);
  if (!cls)
    return out;

  jmethodID method = env->GetStaticMethodID(cls, methodName, "(Ljava/lang/String;)[Ljava/lang/String;");
  if (!method)
    return out;

  jstring arg = env->NewStringUTF(argValue.utf8Ptr());
  jobjectArray result = (jobjectArray)env->CallStaticObjectMethod(cls, method, arg);
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

Maybe<String> AndroidFileAccessBridge::syncBundledAssets(String const& targetRootDirectory) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return {};

  jclass cls = mainActivityClass(env);
  if (!cls)
    return {};

  jmethodID method = env->GetStaticMethodID(cls, "syncBundledAssets", "(Ljava/lang/String;)Ljava/lang/String;");
  if (!method)
    return {};

  jstring arg = env->NewStringUTF(targetRootDirectory.utf8Ptr());
  jstring result = (jstring)env->CallStaticObjectMethod(cls, method, arg);
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

  jclass cls = mainActivityClass(env);
  if (!cls)
    return {};

  jmethodID method = env->GetStaticMethodID(cls, "pickPackedPakAndImport", "(Ljava/lang/String;)Ljava/lang/String;");
  if (!method)
    return {};

  jstring arg = env->NewStringUTF(targetPath.utf8Ptr());
  jstring result = (jstring)env->CallStaticObjectMethod(cls, method, arg);
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

  jclass cls = mainActivityClass(env);
  if (!cls)
    return {};

  jmethodID method = env->GetStaticMethodID(cls, "resolveModsDirectory", "(Ljava/lang/String;)Ljava/lang/String;");
  if (!method)
    return {};

  jstring arg = env->NewStringUTF(fallbackModsDirectory.utf8Ptr());
  jstring result = (jstring)env->CallStaticObjectMethod(cls, method, arg);
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

  jclass cls = mainActivityClass(env);
  if (!cls)
    return false;

  jmethodID method = env->GetStaticMethodID(cls, "openModsDirectory", "(Ljava/lang/String;)Z");
  if (!method)
    return false;

  jstring arg = env->NewStringUTF(modsDirectory.utf8Ptr());
  jboolean result = env->CallStaticBooleanMethod(cls, method, arg);
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

void AndroidFileAccessBridge::showToast(String const& message) {
#ifdef STAR_SYSTEM_ANDROID
  JNIEnv* env = jniEnv();
  if (!env)
    return;

  jclass cls = mainActivityClass(env);
  if (!cls)
    return;

  jmethodID method = env->GetStaticMethodID(cls, "showToast", "(Ljava/lang/String;)V");
  if (!method)
    return;

  jstring arg = env->NewStringUTF(message.utf8Ptr());
  env->CallStaticVoidMethod(cls, method, arg);
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

  jclass cls = mainActivityClass(env);
  if (!cls)
    return;

  jmethodID method = env->GetStaticMethodID(cls, "showDialog", "(Ljava/lang/String;Ljava/lang/String;)V");
  if (!method)
    return;

  jstring jTitle = env->NewStringUTF(title.utf8Ptr());
  jstring jMessage = env->NewStringUTF(message.utf8Ptr());
  env->CallStaticVoidMethod(cls, method, jTitle, jMessage);
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

  jclass cls = mainActivityClass(env);
  if (!cls)
    return false;

  jmethodID method = env->GetStaticMethodID(cls, "openAppSettings", "()Z");
  if (!method)
    return false;

  jboolean result = env->CallStaticBooleanMethod(cls, method);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  return result == JNI_TRUE;
#else
  return false;
#endif
}

}
