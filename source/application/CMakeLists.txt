INCLUDE_DIRECTORIES (
    ${STAR_EXTERN_INCLUDES}
    ${STAR_CORE_INCLUDES}
    ${STAR_PLATFORM_INCLUDES}
    ${STAR_APPLICATION_INCLUDES}
  )

SET (star_application_HEADERS
    StarApplication.hpp
    StarApplicationController.hpp
    StarMainApplication.hpp
    StarRenderer.hpp
  )

SET (star_application_SOURCES
    StarApplication.cpp
    StarRenderer.cpp
  )

SET (star_application_HEADERS ${star_application_HEADERS}
    StarRenderer_opengl.hpp
  )

SET (star_application_SOURCES ${star_application_SOURCES}
    StarRenderer_opengl.cpp
  )

IF (STAR_PLATFORM_MOBILE)
  SET (star_application_HEADERS ${star_application_HEADERS}
      StarPlatformServices_mobile.hpp
      StarRenderer_gles.hpp
      mobile/StarMobileAndroidGyro.hpp
      mobile/StarMobileControls.hpp
      mobile/StarMobileLauncherSupport.hpp
      mobile/StarMobilePlatform.hpp
      mobile/android/StarAndroidFileAccessBridge.hpp
      mobile/ios/StarIosFileAccessBridge.hpp
    )
  SET (star_application_SOURCES ${star_application_SOURCES}
      StarMainApplication_mobile_sdl.cpp
      StarPlatformServices_mobile.cpp
      mobile/StarMobileControls.cpp
      mobile/StarMobileLauncherSupport.cpp
      mobile/StarMobilePlatform.cpp
      mobile/ios/StarIosFileAccessBridge.cpp
      StarRenderer_gles.cpp
    )
  # Android-native translation units (JNI, SDL_system) -- only compiled for a
  # real Android target. On a desktop launcher build (STAR_PC_MOBILE_LAUNCHER)
  # their symbols are only referenced from inside STAR_SYSTEM_ANDROID guards,
  # so they are simply left out.
  IF (STAR_SYSTEM_ANDROID)
    SET (star_application_SOURCES ${star_application_SOURCES}
        mobile/StarMobileAndroidGyro.cpp
        mobile/android/StarAndroidFileAccessBridge.cpp
      )
  ENDIF ()
  IF (STAR_SYSTEM_IOS)
    SET (star_application_HEADERS ${star_application_HEADERS}
        mobile/ios/StarIosVsyncPacer.hpp
      )
    SET (star_application_SOURCES ${star_application_SOURCES}
        # Keep a distinct basename from StarIosFileAccessBridge.cpp to avoid
        # Xcode object-file name collisions in iOS builds.
        mobile/ios/StarIosFileAccessBridgeObjC.mm
        mobile/ios/StarIosVsyncPacerObjC.mm
      )
  ENDIF ()
  IF (STAR_SYSTEM_SWITCH)
    SET (star_application_HEADERS ${star_application_HEADERS}
        mobile/switch/StarSwitchPlatform.hpp
      )
    SET (star_application_SOURCES ${star_application_SOURCES}
        mobile/switch/StarSwitchPlatform.cpp
        mobile/switch/StarSwitchCompat.c
      )
  ENDIF ()
ELSE ()
  SET (star_application_HEADERS ${star_application_HEADERS}
      StarP2PNetworkingService_pc.hpp
      StarPlatformServices_pc.hpp
    )
  SET (star_application_SOURCES ${star_application_SOURCES}
      StarMainApplication_sdl.cpp
      StarP2PNetworkingService_pc.cpp
      StarPlatformServices_pc.cpp
    )
ENDIF ()

IF (STAR_ENABLE_STEAM_INTEGRATION)
  SET (star_application_HEADERS ${star_application_HEADERS}
      StarDesktopService_pc_steam.hpp
      StarStatisticsService_pc_steam.hpp
      StarUserGeneratedContentService_pc_steam.hpp
    )
  SET (star_application_SOURCES ${star_application_SOURCES}
      StarDesktopService_pc_steam.cpp
      StarStatisticsService_pc_steam.cpp
      StarUserGeneratedContentService_pc_steam.cpp
    )
ENDIF ()

IF (STAR_ENABLE_DISCORD_INTEGRATION)
  SET (star_application_SOURCES ${star_application_SOURCES}
      discord/core.cpp
      discord/image_manager.cpp
      discord/lobby_manager.cpp
      discord/network_manager.cpp
      discord/overlay_manager.cpp
      discord/relationship_manager.cpp
      discord/storage_manager.cpp
      discord/store_manager.cpp
      discord/types.cpp
      discord/user_manager.cpp
      discord/voice_manager.cpp
      discord/achievement_manager.cpp
      discord/activity_manager.cpp
      discord/application_manager.cpp
    )
ENDIF ()

ADD_LIBRARY (star_application OBJECT ${star_application_SOURCES} ${star_application_HEADERS})

IF (STAR_SYSTEM_IOS)
  TARGET_COMPILE_OPTIONS(star_application PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
ENDIF ()

IF(STAR_PRECOMPILED_HEADERS)
  TARGET_PRECOMPILE_HEADERS (star_application REUSE_FROM star_core)
ENDIF()
