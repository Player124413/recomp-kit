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
              --source ${RECOMP_DEVELOPER_GAME_DIR}
              --dest $<TARGET_BUNDLE_CONTENT_DIR:${target}>/game
      WORKING_DIRECTORY ${POP_ROOT}
      COMMENT "Staging game files into ${RECOMP_APP_NAME}.app/game"
      VERBATIM)
    # The app icon is the game's own: the executable's icon group, scaled.
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/extract_icon.py
              --exe ${RECOMP_DEVELOPER_EXE}
              --dest $<TARGET_BUNDLE_CONTENT_DIR:${target}>
      WORKING_DIRECTORY ${POP_ROOT}
      COMMENT "Extracting the app icon from the game executable"
      VERBATIM)
  endif()
  # classic-modes.json is one resource the host reads at startup; the
  # translation's symbol table is the other (the mod foundation's settings
  # store initialises only after it loads).
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${POP_ROOT}/tools/recomp/baseline/classic-modes.json
            $<TARGET_BUNDLE_CONTENT_DIR:${target}>/classic-modes.json
    VERBATIM)
  if(EXISTS ${POP_BUILD_ROOT}/recomp/symbols.json)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${POP_BUILD_ROOT}/recomp/symbols.json
              $<TARGET_BUNDLE_CONTENT_DIR:${target}>/symbols.json
      VERBATIM)
  endif()
endfunction()
