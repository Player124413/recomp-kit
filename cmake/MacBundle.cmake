# pop_mac_bundle(<target>): make <target> a .app in build/, named by
# RECOMP_APP_NAME, with Info.plist copied verbatim and the resources,
# core mods, texture pack and ad-hoc signature applied after the link.
function(pop_mac_bundle target)
  configure_file(${POP_ROOT}/host/Info.plist.in ${CMAKE_BINARY_DIR}/generated/Info.plist @ONLY)
  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE ON
    OUTPUT_NAME ${RECOMP_APP_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${POP_BUILD_DIR}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/generated/Info.plist)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/recomp/finish_bundle.py
            --bundle ${POP_BUILD_DIR}/${RECOMP_APP_NAME}.app
            --name ${RECOMP_APP_NAME} --cc ${CMAKE_C_COMPILER} --version ${POP_RECOMP_VERSION}
            --build-root ${POP_BUILD_ROOT} --game-dir ${RECOMP_GAME_DIR}
    WORKING_DIRECTORY ${POP_ROOT}
    COMMENT "Finishing ${RECOMP_APP_NAME}.app"
    VERBATIM)
endfunction()
