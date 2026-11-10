# Keep IPA staging independent of Xcode so fresh/repeated packaging can also
# be checked on a host without the Apple toolchain.
FUNCTION(star_add_ios_ipa_target app_target app_bundle)
  SET(STAR_IOS_IPA_DIR "${CMAKE_BINARY_DIR}/ipa")
  SET(STAR_IOS_IPA_PAYLOAD_DIR "${STAR_IOS_IPA_DIR}/Payload")
  SET(STAR_IOS_IPA_PAYLOAD "${STAR_IOS_IPA_PAYLOAD_DIR}/OpenStarbound.app")
  SET(STAR_IOS_IPA_FILE "${STAR_IOS_IPA_DIR}/OpenStarbound-iOS-Release-unsigned.ipa")

  # Recreate Payload from the current app bundle; never retain stale binaries.
  ADD_CUSTOM_TARGET(ipa
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_IOS_IPA_DIR}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${STAR_IOS_IPA_PAYLOAD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${STAR_IOS_IPA_FILE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAR_IOS_IPA_PAYLOAD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${app_bundle}" "${STAR_IOS_IPA_PAYLOAD}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${STAR_IOS_IPA_DIR}" zip -qry "${STAR_IOS_IPA_FILE}" "Payload"
    DEPENDS ${app_target}
    # Generators enter WORKING_DIRECTORY before executing the first command.
    # The IPA directory does not exist yet on a clean build.
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Packaging unsigned IPA: ipa/OpenStarbound-iOS-Release-unsigned.ipa"
    VERBATIM
  )
ENDFUNCTION()
