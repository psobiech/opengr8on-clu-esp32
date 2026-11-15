/**
 * @file luac_host.c
 * @brief Build-time Lua compiler: loads a Lua source and writes it as stripped bytecode.
 *
 * Built for the build host by project_include.cmake from the same Lua sources and number
 * configuration (LUA_32BITS / LUA_FLOAT_TYPE) as the firmware, so the bytecode header matches the
 * target VM. Usage: luac_host <input.lua> <output> <chunkname>
 */
#include <stdio.h>
#include <stdlib.h>

#include "lauxlib.h"
#include "lua.h"

static int write_chunk(lua_State *L, const void *p, size_t size, void *ud)
{
    (void)L;
    return fwrite(p, 1, size, (FILE *)ud) != size;
}

static int read_file(const char *path, char **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc(size > 0 ? (size_t)size : 1);
    *len = *out ? fread(*out, 1, (size_t)size, f) : 0;
    fclose(f);
    return *out && *len == (size_t)size;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <input.lua> <output> <chunkname>\n", argv[0]);
        return 2;
    }
    char *src = NULL;
    size_t len = 0;
    if (!read_file(argv[1], &src, &len)) {
        fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 1;
    }
    lua_State *L = luaL_newstate();
    if (luaL_loadbufferx(L, src, len, argv[3], "t") != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "%s: cannot write\n", argv[2]);
        return 1;
    }
    int failed = lua_dump(L, write_chunk, out, 1) != 0;
    failed |= fclose(out) != 0;
    lua_close(L);
    free(src);
    if (failed) {
        remove(argv[2]);
    }
    return failed;
}
