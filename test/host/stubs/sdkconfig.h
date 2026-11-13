// Host stub: the Kconfig values the host-tested sources read. A test may define any of them first
// (e.g. bound to a variable, to cover several configurations in one binary).
#pragma once
#ifndef CONFIG_CLU_SOFT_RESET
#define CONFIG_CLU_SOFT_RESET 1
#endif
// Reported firmware identity (main/clu_state.h takes these over its per-device constants)
#ifndef CLU_FW_API_VER
#define CLU_FW_API_VER 5
#endif
#ifndef CLU_FW_VERSION
#define CLU_FW_VERSION "1.0.0"
#endif
#ifndef CONFIG_CLU_SERIAL
#define CONFIG_CLU_SERIAL ""
#endif
#ifndef CONFIG_CLU_PRIVATE_KEY
#define CONFIG_CLU_PRIVATE_KEY ""
#endif
#ifndef CONFIG_CLU_IV
#define CONFIG_CLU_IV ""
#endif
#ifndef CONFIG_CLU_SETUP_WINDOW_S
#define CONFIG_CLU_SETUP_WINDOW_S 60
#endif
#ifndef CONFIG_CLU_TFBUS
#define CONFIG_CLU_TFBUS 1
#endif
#ifndef CONFIG_CLU_TFBUS_MASTER
#if CONFIG_CLU_TFBUS || CONFIG_CLU_MYSENSORS
#define CONFIG_CLU_TFBUS_MASTER 1
#else
#define CONFIG_CLU_TFBUS_MASTER 0
#endif
#endif
#ifndef CONFIG_CLU_RTC
#define CONFIG_CLU_RTC 1
#endif
#ifndef CONFIG_CLU_RTC_SDA_GPIO
#define CONFIG_CLU_RTC_SDA_GPIO 39
#endif
#ifndef CONFIG_CLU_RTC_SCL_GPIO
#define CONFIG_CLU_RTC_SCL_GPIO 38
#endif
#ifndef CONFIG_CLU_SETUP_BUTTON_GPIO
#define CONFIG_CLU_SETUP_BUTTON_GPIO 0
#endif
#ifndef CONFIG_CLU_WIFI
#define CONFIG_CLU_WIFI 1
#endif
