cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Put "OpenGr8on ESP32 CLU" at the top of menuconfig: IDF sources Kconfig.projbuild menus only
# after its own "Build type" menu, so wrap the IDF root Kconfig instead (main/Kconfig.clu first).
set(CLU_ROOT_KCONFIG "${CMAKE_BINARY_DIR}/Kconfig.root")
file(WRITE "${CLU_ROOT_KCONFIG}.tmp"
    "mainmenu \"OpenGr8on ESP32 CLU Configuration\"\n"
    "source \"${CMAKE_CURRENT_LIST_DIR}/main/Kconfig.clu\"\n"
    "source \"$ENV{IDF_PATH}/Kconfig\"\n")
configure_file("${CLU_ROOT_KCONFIG}.tmp" "${CLU_ROOT_KCONFIG}" COPYONLY) # rewrite only on change
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/main/Kconfig.clu")
idf_build_set_property(__ROOT_KCONFIG "${CLU_ROOT_KCONFIG}")

project(opengr8on_clu_esp32)
