INCLUDE_DIRECTORIES (
    ${STAR_EXTERN_INCLUDES}
    ${STAR_CORE_INCLUDES}
    ${STAR_BASE_INCLUDES}
    ${STAR_GAME_INCLUDES}
    ${STAR_PLATFORM_INCLUDES}
    ${STAR_APPLICATION_INCLUDES}
    ${STAR_RENDERING_INCLUDES}
    ${STAR_WINDOWING_INCLUDES}
    ${STAR_FRONTEND_INCLUDES}
  )

SET (star_client_HEADERS
      StarClientApplication.hpp
      StarRenderingLuaBindings.hpp
    )

SET (star_client_SOURCES
      StarClientApplication.cpp
      StarRenderingLuaBindings.cpp
    )

IF (STAR_SYSTEM_WINDOWS)
  SET (star_client_RESOURCES
      starbound.rc
    )
ENDIF ()

IF (STAR_SYSTEM_IOS)
  SET(STAR_IOS_ASSET_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/ios/Assets.xcassets")
  SET_SOURCE_FILES_PROPERTIES("${STAR_IOS_ASSET_CATALOG}" PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
  )
  SET(star_client_RESOURCES
      ${star_client_RESOURCES}
      "${STAR_IOS_ASSET_CATALOG}"
    )
ENDIF ()

IF (STAR_SYSTEM_ANDROID)
  ADD_LIBRARY (starbound SHARED
    $<TARGET_OBJECTS:star_extern> $<TARGET_OBJECTS:star_core> $<TARGET_OBJECTS:star_base> $<TARGET_OBJECTS:star_game>
    $<TARGET_OBJECTS:star_application> $<TARGET_OBJECTS:star_rendering> $<TARGET_OBJECTS:star_windowing> $<TARGET_OBJECTS:star_frontend>
    ${star_client_HEADERS} ${star_client_SOURCES} ${star_client_RESOURCES})
ELSEIF (STAR_SYSTEM_IOS)
  ADD_EXECUTABLE (starbound MACOSX_BUNDLE
    $<TARGET_OBJECTS:star_extern> $<TARGET_OBJECTS:star_core> $<TARGET_OBJECTS:star_base> $<TARGET_OBJECTS:star_game>
    $<TARGET_OBJECTS:star_application> $<TARGET_OBJECTS:star_rendering> $<TARGET_OBJECTS:star_windowing> $<TARGET_OBJECTS:star_frontend>
    ${star_client_HEADERS} ${star_client_SOURCES} ${star_client_RESOURCES})
ELSE ()
  ADD_EXECUTABLE (starbound WIN32
    $<TARGET_OBJECTS:star_extern> $<TARGET_OBJECTS:star_core> $<TARGET_OBJECTS:star_base> $<TARGET_OBJECTS:star_game>
    $<TARGET_OBJECTS:star_application> $<TARGET_OBJECTS:star_rendering> $<TARGET_OBJECTS:star_windowing> $<TARGET_OBJECTS:star_frontend>
    ${star_client_HEADERS} ${star_client_SOURCES} ${star_client_RESOURCES})
ENDIF ()

IF (STAR_PRECOMPILED_HEADERS)
  TARGET_PRECOMPILE_HEADERS (starbound REUSE_FROM star_core)
ENDIF()

IF(STAR_SYSTEM_MACOS)
  SET_TARGET_PROPERTIES (starbound PROPERTIES LINK_FLAGS "-Wl,-rpath,'@executable_path'")
ELSEIF(STAR_SYSTEM_IOS)
  # iOS uses Apple's linker and does not support GNU ld options such as
  # --export-dynamic. Keep link flags empty here.
ELSEIF (UNIX)
  SET_TARGET_PROPERTIES (starbound PROPERTIES LINK_FLAGS "-Wl,--export-dynamic,-rpath,'$ORIGIN'")
ENDIF()

IF (STAR_SYSTEM_ANDROID)
  SET (STAR_ANDROID_JNI_LIB_DIR "${PROJECT_SOURCE_DIR}/application/mobile/android/host/app/src/main/jniLibs/arm64-v8a")
  SET_TARGET_PROPERTIES (starbound PROPERTIES OUTPUT_NAME main)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_BUILD_TYPE STREQUAL "RelWithAsserts" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    if(CMAKE_STRIP)
      ADD_CUSTOM_COMMAND(TARGET starbound POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_ANDROID_JNI_LIB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:starbound>" "${STAR_ANDROID_JNI_LIB_DIR}/libmain.so"
        COMMAND ${CMAKE_STRIP} --strip-unneeded "${STAR_ANDROID_JNI_LIB_DIR}/libmain.so"
        COMMENT "Copying and stripping libmain.so into Android host jniLibs"
        VERBATIM
      )
    else()
      ADD_CUSTOM_COMMAND(TARGET starbound POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_ANDROID_JNI_LIB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:starbound>" "${STAR_ANDROID_JNI_LIB_DIR}/libmain.so"
        COMMENT "Copying libmain.so into Android host jniLibs (strip unavailable)"
        VERBATIM
      )
    endif()
  else()
    ADD_CUSTOM_COMMAND(TARGET starbound POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_ANDROID_JNI_LIB_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:starbound>" "${STAR_ANDROID_JNI_LIB_DIR}/libmain.so"
      COMMENT "Copying libmain.so into Android host jniLibs"
      VERBATIM
    )
  endif()
ENDIF ()

IF (STAR_SYSTEM_IOS)
  # Info.plist is generated from this template. It bakes in UILaunchScreen (required
  # on iOS 13+ for full-screen treatment) and UISupportedInterfaceOrientations (all
  # four, so the app can rotate freely). Xcode substitutes $(PRODUCT_BUNDLE_IDENTIFIER),
  # $(MARKETING_VERSION), $(CURRENT_PROJECT_VERSION), and $(EXECUTABLE_NAME) at build time.
  SET_TARGET_PROPERTIES(starbound PROPERTIES
    OUTPUT_NAME "OpenStarbound"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.devoflife.osbm.ios"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_PRODUCT_NAME "OpenStarbound"
    XCODE_ATTRIBUTE_MARKETING_VERSION "1.0"
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "1"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
  )

  SET(STAR_IOS_BUNDLE_ASSETS_SOURCE "${PROJECT_SOURCE_DIR}/../assets/opensb")
  SET(STAR_IOS_BUNDLE_ASSETS_DEST "$<TARGET_BUNDLE_DIR:starbound>/assets/opensb")
  SET(STAR_IOS_BUNDLE_LANG_SOURCE "${PROJECT_SOURCE_DIR}/application/mobile/android/host/app/src/main/assets/lang")
  SET(STAR_IOS_BUNDLE_LANG_DEST "$<TARGET_BUNDLE_DIR:starbound>/assets/lang")

  ADD_CUSTOM_COMMAND(TARGET starbound POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_IOS_BUNDLE_ASSETS_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${STAR_IOS_BUNDLE_ASSETS_SOURCE}" "${STAR_IOS_BUNDLE_ASSETS_DEST}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_IOS_BUNDLE_LANG_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${STAR_IOS_BUNDLE_LANG_SOURCE}" "${STAR_IOS_BUNDLE_LANG_DEST}"
    COMMENT "Copying OpenStarbound bundled assets into iOS app bundle"
    VERBATIM
  )

  # IPA packaging
  # Only the 'ipa' target writes Payload contents. It always recreates Payload
  # from the current app bundle so stale app bundles or binaries cannot survive
  # from previous packaging runs.
  SET(STAR_IOS_IPA_DIR      "${CMAKE_BINARY_DIR}/ipa")
  SET(STAR_IOS_IPA_PAYLOAD_DIR "${STAR_IOS_IPA_DIR}/Payload")
  SET(STAR_IOS_IPA_PAYLOAD  "${STAR_IOS_IPA_PAYLOAD_DIR}/OpenStarbound.app")
  SET(STAR_IOS_IPA_FILE     "${STAR_IOS_IPA_DIR}/OpenStarbound-iOS-Release-unsigned.ipa")

  # 'ipa' target: rebuilds a fresh unsigned .ipa from the current app bundle.
  # Depends on 'starbound' so the binary is always up to date before packaging.
  # Usage: cmake --build <build-dir> --config Release --target ipa
  ADD_CUSTOM_TARGET(ipa
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_IOS_IPA_DIR}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf         "${STAR_IOS_IPA_PAYLOAD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E rm -f          "${STAR_IOS_IPA_FILE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_IOS_IPA_PAYLOAD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "$<TARGET_BUNDLE_DIR:starbound>"
      "${STAR_IOS_IPA_PAYLOAD}"
    COMMAND zip -qry "${STAR_IOS_IPA_FILE}" "Payload"
    DEPENDS starbound
    WORKING_DIRECTORY "${STAR_IOS_IPA_DIR}"
    COMMENT "Packaging unsigned IPA: ipa/OpenStarbound-iOS-Release-unsigned.ipa"
    VERBATIM
  )
ENDIF ()

IF (STAR_SYSTEM_MACOS)
  SET_TARGET_PROPERTIES (starbound PROPERTIES XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES)
ENDIF()

TARGET_LINK_LIBRARIES (starbound ${STAR_EXT_LIBS} ${STAR_EXT_GUI_LIBS})
