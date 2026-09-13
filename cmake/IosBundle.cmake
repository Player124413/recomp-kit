# pop_ios_bundle(<target>): an iPad app under build/ios, signed automatically by
# Xcode with RECOMP_IOS_TEAM, carrying the game files under game/.
function(pop_ios_bundle target)
  configure_file(${POP_ROOT}/host/Info-ios.plist.in ${CMAKE_BINARY_DIR}/generated/Info-ios.plist @ONLY)
  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE ON
    OUTPUT_NAME ${RECOMP_APP_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${POP_BUILD_DIR}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/generated/Info-ios.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${RECOMP_BUNDLE_ID}
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${RECOMP_IOS_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 17.0
    XCODE_ATTRIBUTE_ENABLE_BITCODE NO
    "XCODE_ATTRIBUTE_INFOPLIST_KEY_UISupportedInterfaceOrientations~ipad"
      "UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight")
  # The game itself, filtered by games/<id>/game.toml [bundle].exclude. A stub
  # build has no game to bundle.
  if(NOT POP_TRANSLATE STREQUAL "STUB")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/stage_game_files.py
              --game-dir ${RECOMP_GAME_DIR}
              --source ${POP_ROOT}/${RECOMP_DEVELOPER_GAME_DIR}
              --dest $<TARGET_BUNDLE_CONTENT_DIR:${target}>/game
      WORKING_DIRECTORY ${POP_ROOT}
      COMMENT "Staging game files into ${RECOMP_APP_NAME}.app/game"
      VERBATIM)
  endif()
  # classic-modes.json is the one resource the host reads at startup.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${POP_ROOT}/tools/recomp/baseline/classic-modes.json
            $<TARGET_BUNDLE_CONTENT_DIR:${target}>/classic-modes.json
    VERBATIM)
endfunction()
