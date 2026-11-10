// Force-included (-include) into firmware sources built for host tests (not into the tests):
//  - file calls on the device's VFS ("/fs/...") go through host_* wrappers that the test maps to
//    a temporary directory;
//  - the CLU identity Kconfig strings become variables, so one binary covers every configuration.
#pragma once
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

FILE *host_fopen(const char *path, const char *mode);
int host_mkdir(const char *path, mode_t mode);
int host_stat(const char *path, struct stat *st);
#define fopen host_fopen
#define mkdir host_mkdir
#define stat(p, s) host_stat(p, s)

extern const char *host_cfg_serial, *host_cfg_pin, *host_cfg_iv;
#define CONFIG_CLU_SERIAL      host_cfg_serial
#define CONFIG_CLU_PRIVATE_KEY host_cfg_pin
#define CONFIG_CLU_IV          host_cfg_iv
