/**
 * @file clu_time.h
 * @brief Time zone and SNTP configuration (CLU features 22 NTPServer / 23 TimeZone).
 */
#pragma once

#include <stdbool.h>

/**
 * @brief Load persisted time zone (TZ env) and NTP server; set the system clock from the RTC.
 *
 * Call after nvs_flash_init() and before the Lua runtime starts.
 */
void clu_time_init(void);

/** @brief Write the system clock to the RTC (after it was set by hand, e.g. SetDateTime). */
void clu_time_store(void);

/** @brief Start SNTP. Needs lwIP (esp_netif_init()), not an IP: it retries until it gets one. */
void clu_time_start_sntp(void);

/**
 * @brief Switch SNTP on / off (GATE feature 21 UseNTP). Not persisted: OnInit sets it each boot.
 * @param on false stops SNTP (the clock then runs from SetDateTime / the RTC only).
 */
void clu_time_set_ntp_enabled(bool on);

/**
 * @brief Set the NTP server (CLU feature 22 NTPServer).
 *
 * Persisted; no-op if unchanged.
 *
 * @param server Host name or IP address.
 */
void clu_time_set_ntp_server(const char *server);

/**
 * @brief Set the time zone (CLU feature 23 TimeZone).
 *
 * Persisted; no-op if unchanged.
 *
 * @param zone OM time zone enum index.
 */
void clu_time_set_zone(int zone);
