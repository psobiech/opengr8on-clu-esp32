cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Put our options (main/Kconfig.clu) at the menuconfig root and every ESP-IDF / component option
# under "Advanced": IDF sources Kconfig.projbuild menus only after its own "Build type" menu, so wrap
# the IDF root Kconfig instead. IDF's own mainmenu line inside it only renames the title.
set(CLU_ROOT_KCONFIG "${CMAKE_BINARY_DIR}/Kconfig.root")
file(WRITE "${CLU_ROOT_KCONFIG}.tmp"
    "mainmenu \"OpenGr8on ESP32 CLU Configuration\"\n"
    "source \"${CMAKE_CURRENT_LIST_DIR}/main/Kconfig.clu\"\n"
    "menu \"Advanced (ESP-IDF)\"\n"
    "source \"$ENV{IDF_PATH}/Kconfig\"\n"
    "endmenu\n")
configure_file("${CLU_ROOT_KCONFIG}.tmp" "${CLU_ROOT_KCONFIG}" COPYONLY) # rewrite only on change
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/main/Kconfig.clu")
idf_build_set_property(__ROOT_KCONFIG "${CLU_ROOT_KCONFIG}")

project(opengr8on_clu_esp32)

# float is FPU-backed on ESP32/ESP32-S3, double is soft-float: reject silent float -> double promotion
# in our own code (not the vendored Lua sources).
file(GLOB CLU_OWN_COMPONENTS RELATIVE "${CMAKE_CURRENT_LIST_DIR}/components" "${CMAKE_CURRENT_LIST_DIR}/components/*")
list(REMOVE_ITEM CLU_OWN_COMPONENTS lua)
foreach(comp main ${CLU_OWN_COMPONENTS})
    if(TARGET __idf_${comp})
        target_compile_options(__idf_${comp} PRIVATE -Werror=double-promotion)
    endif()
endforeach()
