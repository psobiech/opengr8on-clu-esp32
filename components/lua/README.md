Upstream Lua sources from https://www.lua.org/ftp/, `src/` unmodified except removal of the
standalone interpreters `lua.c` and `luac.c`. MIT license, see LICENSE.

| Dir | Release | sha256 of tarball |
|---|---|---|
| `src-5.3/` (default) | lua-5.3.6.tar.gz | fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60 |
| `src-5.5/` | lua-5.5.1.tar.gz | 1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce |

Selected by menuconfig `CLU_LUA_VERSION`. The real CLU firmware embeds Lua 5.3, so 5.3.6 is the
default. 5.5 differences that can break CLU scripts: `global` is a reserved word, for-loop
variables are read-only, no `bit32` (5.3 here is built with `LUA_COMPAT_5_2`), new
`math.random` generator, string→number coercion via string metamethods.
