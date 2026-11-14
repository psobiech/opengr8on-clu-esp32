idf_component_register(
    SRCS "clu_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES clu_lua lua
    PRIV_REQUIRES mqtt mbedtls
)

# ESP-IDF first evaluates this file in script mode to collect requirements; build commands
# (add_custom_command, target_*) are only valid in the real configure pass.
if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

# mqtt.lua is generated from the ordered parts in lua/parts.txt (see join_lua.cmake).
set(MQTT_LUA_DIR "${CMAKE_CURRENT_LIST_DIR}/lua")
set(MQTT_LUA_PARTS_FILE "${MQTT_LUA_DIR}/parts.txt")
file(STRINGS "${MQTT_LUA_PARTS_FILE}" MQTT_LUA_PART_NAMES)
set(MQTT_LUA_PARTS)
foreach(part IN LISTS MQTT_LUA_PART_NAMES)
    if(NOT part STREQUAL "")
        list(APPEND MQTT_LUA_PARTS "${MQTT_LUA_DIR}/${part}")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MQTT_LUA_PARTS_FILE}")

set(MQTT_LUA_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/mqtt.lua")
add_custom_command(
    OUTPUT "${MQTT_LUA_GENERATED}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${MQTT_LUA_GENERATED}"
            "-DPARTS_FILE=${MQTT_LUA_PARTS_FILE}" "-DSOURCE_DIR=${MQTT_LUA_DIR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/join_lua.cmake"
    DEPENDS "${MQTT_LUA_PARTS_FILE}" ${MQTT_LUA_PARTS} "${CMAKE_CURRENT_LIST_DIR}/join_lua.cmake"
    VERBATIM
)
target_add_binary_data(${COMPONENT_LIB} "${MQTT_LUA_GENERATED}" TEXT)
