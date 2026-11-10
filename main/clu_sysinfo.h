/**
 * @file clu_sysinfo.h
 * @brief Firmware internals exposed to Lua.
 *
 * Heap, WiFi, reset reason, TFBUS stats and restart are exposed to Lua as __clu functions;
 * mqtt.lua turns them into Home Assistant diagnostic entities.
 */
#pragma once

/** @brief Register the __clu functions. Call before clu_lua_start(). */
void clu_sysinfo_init(void);
