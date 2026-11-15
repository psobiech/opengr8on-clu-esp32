# clu_lua_embed(<target> <source.lua>): embeds a Lua chunk into <target> as
# _binary_<name>_start / _end (NUL-terminated, like EMBED_TXTFILES, so the length is end - start - 1).
#
# With CONFIG_CLU_LUA_PRECOMPILE the chunk is compiled on the build host to stripped bytecode
# (~40% smaller than the source, no parsing at boot); otherwise the source text is embedded.
# Loaders must accept binary chunks (luaL_loadbufferx mode "bt").

set(CLU_LUA_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Builds the host compiler once per configuration (Lua version + number type); sets <out_var>.
function(__clu_luac_host out_var)
    if(CONFIG_CLU_LUA_5_5)
        set(version "5.5")
    else()
        set(version "5.3")
    endif()
    set(lua_dir "${CLU_LUA_COMPONENT_DIR}/src-${version}")
    set(defines)
    if(NOT CONFIG_CLU_LUA_5_5)
        list(APPEND defines "-DLUA_COMPAT_5_2")
    endif()
    if(CONFIG_CLU_LUA_32BITS)
        list(APPEND defines "-DLUA_32BITS=1")
    elseif(CONFIG_CLU_LUA_FLOAT)
        list(APPEND defines "-DLUA_FLOAT_TYPE=LUA_FLOAT_FLOAT")
    endif()

    file(GLOB lua_srcs "${lua_dir}/*.c")
    set(tool_src "${CLU_LUA_COMPONENT_DIR}/tools/luac_host.c")
    set(hash_input "${version};${defines}")
    foreach(f IN LISTS lua_srcs tool_src)
        file(MD5 "${f}" f_md5)
        string(APPEND hash_input ";${f_md5}")
    endforeach()
    string(MD5 key "${hash_input}")
    set(work_dir "${CMAKE_BINARY_DIR}/luac_host")
    set(luac "${work_dir}/luac-${key}${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(${out_var} "${luac}" PARENT_SCOPE)
    if(EXISTS "${luac}")
        return()
    endif()
    file(MAKE_DIRECTORY "${work_dir}")

    if(NOT CLU_HOST_CC)
        find_program(CLU_HOST_CC NAMES cc gcc clang NO_CMAKE_FIND_ROOT_PATH)
    endif()
    if(NOT CLU_HOST_CC)
        message(FATAL_ERROR "CONFIG_CLU_LUA_PRECOMPILE needs a host C compiler (cc, gcc or clang) "
                            "on PATH, or set -DCLU_HOST_CC=<path>; or disable it in menuconfig.")
    endif()

    # Lua 5.3 bytecode records sizeof(size_t) and stores long string lengths as size_t: dump them as
    # the target's 4 bytes instead of the (64-bit) host's. Upstream ldump.c stays untouched.
    if(version STREQUAL "5.3")
        file(READ "${lua_dir}/ldump.c" ldump)
        set(patched "${ldump}")
        string(REPLACE "DumpByte(sizeof(size_t), D);" "DumpByte(4, D);" patched "${patched}")
        string(REPLACE "      DumpVar(size, D);"
                       "      { unsigned int size32 = (unsigned int)size; DumpVar(size32, D); }"
                       patched "${patched}")
        if(patched STREQUAL ldump)
            message(FATAL_ERROR "luac_host: could not patch ${lua_dir}/ldump.c for a 32-bit target")
        endif()
        file(WRITE "${work_dir}/ldump.c" "${patched}")
        list(REMOVE_ITEM lua_srcs "${lua_dir}/ldump.c")
        list(APPEND lua_srcs "${work_dir}/ldump.c")
    endif()

    message(STATUS "Building host Lua ${version} compiler: ${luac}")
    execute_process(
        COMMAND "${CLU_HOST_CC}" -O1 -w ${defines} "-I${lua_dir}" -o "${luac}" "${tool_src}" ${lua_srcs} -lm
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        file(REMOVE "${luac}")
        message(FATAL_ERROR "Building the host Lua compiler failed:\n${out}")
    endif()
endfunction()

function(clu_lua_embed target source)
    if(NOT CONFIG_CLU_LUA_PRECOMPILE)
        target_add_binary_data(${target} "${source}" TEXT)
        return()
    endif()
    __clu_luac_host(luac)
    get_filename_component(name "${source}" NAME)
    get_filename_component(stem "${source}" NAME_WE)
    # Same file name in a separate directory: the embedded symbol names come from it.
    set(output "${CMAKE_CURRENT_BINARY_DIR}/luac/${name}")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/luac"
        COMMAND "${luac}" "${source}" "${output}" "=${stem}"
        DEPENDS "${source}" "${luac}"
        COMMENT "Compiling ${name} to Lua bytecode"
        VERBATIM)
    target_add_binary_data(${target} "${output}" TEXT)
endfunction()
