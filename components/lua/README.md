Upstream Lua sources from https://www.lua.org/ftp/, with the standalone interpreters `lua.c` and
`luac.c` removed. Lua 5.5's `luaconf.h` has a small local change to allow overriding its numeric
type macros, matching the configuration points already supported by Lua 5.3. MIT license, see
LICENSE.

| Dir | Release | sha256 of tarball |
|---|---|---|
| `src-5.3/` (default) | lua-5.3.6.tar.gz | fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60 |
| `src-5.5/` | lua-5.5.1.tar.gz | 1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce |

Selected by menuconfig `CLU_LUA_VERSION`. The real CLU firmware embeds Lua 5.3, so 5.3.6 is the
default. By default, Lua uses single-precision floats and 64-bit integers (`CLU_LUA_FLOAT`). Floats
retain about 7 significant digits; integer serial numbers and IDs retain their full 64-bit range.
`CLU_LUA_32BITS` instead builds either version with both 32-bit integers and floats; serial numbers
at or above 2^31 become negative and `millis` exceeds the signed integer range after about 24.9
days. Firmware C interfaces also use `float` for fractional values.

5.5 differences that can break CLU scripts: `global` is a reserved word, for-loop variables are
read-only, no `bit32` (5.3 here is built with `LUA_COMPAT_5_2`), new `math.random` generator,
string→number coercion via string metamethods.
