# Unmodified upstream Lua sources from lua.org (standalone lua.c/luac.c removed).
# Version selected in menuconfig: 5.3.6 (default, same as the CLU firmware) or 5.5.1.
if(CONFIG_CLU_LUA_5_5)
    set(LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src-5.5")
else()
    set(LUA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src-5.3")
endif()
file(GLOB LUA_SRCS "${LUA_DIR}/*.c")
idf_component_register(
    SRCS ${LUA_SRCS}
    INCLUDE_DIRS "${LUA_DIR}"
)
if(NOT CONFIG_CLU_LUA_5_5)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC LUA_COMPAT_5_2)
endif()
# C recursion limits (parser nesting, C calls, pattern matching) sized for the 48 KB Lua task
# stack (clu_lua.c). Upstream 200 needs ~50 KB of stack for the parser alone; 120 levels are far
# beyond what OM-generated or hand-written CLU scripts use. Exceeding it is a Lua error, not a crash.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUAI_MAXCCALLS=120 MAXCCALLS=120)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-implicit-fallthrough -Wno-maybe-uninitialized -O2)
