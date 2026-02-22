#include "StarLuaConverters.hpp"
#include "StarVoiceLuaBindings.hpp"
#include "StarVoice.hpp"


namespace Star {

typedef Voice::SpeakerId SpeakerId;
LuaCallbacks LuaBindings::makeVoiceCallbacks() {
  LuaCallbacks callbacks;

  callbacks.registerCallbackWithSignature<StringList>("devices", []() -> StringList {
    if (auto voice = Voice::singletonPtr())
      return voice->availableDevices();
    return {};
  });

  callbacks.registerCallback("getSettings", []() -> Json {
    if (auto voice = Voice::singletonPtr())
      return voice->saveJson();
    return JsonObject{};
  });

  callbacks.registerCallback("mergeSettings", [](Json const& settings) {
    if (auto voice = Voice::singletonPtr())
      voice->loadJson(settings);
  });

  callbacks.registerCallback("setSpeakerMuted", [](SpeakerId speakerId, bool muted) {
    if (auto voice = Voice::singletonPtr())
      voice->speaker(speakerId)->muted = muted;
  });

  callbacks.registerCallback("speakerMuted", [](SpeakerId speakerId) {
    if (auto voice = Voice::singletonPtr())
      return (bool)voice->speaker(speakerId)->muted;
    return false;
  });

  callbacks.registerCallback("setSpeakerVolume", [](SpeakerId speakerId, float volume) {
    if (auto voice = Voice::singletonPtr())
      voice->speaker(speakerId)->volume = volume;
  });

  callbacks.registerCallback("speakerVolume", [](SpeakerId speakerId) {
    if (auto voice = Voice::singletonPtr())
      return (float)voice->speaker(speakerId)->volume;
    return 0.0f;
  });

  callbacks.registerCallback("speakerPosition", [](SpeakerId speakerId) {
    if (auto voice = Voice::singletonPtr())
      return voice->speaker(speakerId)->position;
    return Vec2F();
  });

  callbacks.registerCallback("speaker", [](Maybe<SpeakerId> speakerId) -> Json {
    if (auto voice = Voice::singletonPtr()) {
      if (speakerId)
        return voice->speaker(*speakerId)->toJson();
      else
        return voice->localSpeaker()->toJson();
    }
    return JsonObject{};
  });

  callbacks.registerCallback("speakers", [](Maybe<bool> onlyPlaying) -> List<Json> {
    List<Json> list;
    auto voice = Voice::singletonPtr();
    if (!voice)
      return list;

    for (auto& speaker : voice->sortedSpeakers(onlyPlaying.value(true)))
      list.append(speaker->toJson());

    return list;
  });

  return callbacks;
}

}
