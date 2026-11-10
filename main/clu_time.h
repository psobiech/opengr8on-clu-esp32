/**
 * @file clu_time.h
 * @brief Time zone and SNTP configuration (CLU features 22 NTPServer / 23 TimeZone).
 */
#pragma once

/** @brief Load persisted time zone (TZ env) and NTP server. Call after nvs_flash_init(). */
void clu_time_init(void);

/** @brief Start SNTP (after the network is up). */
void clu_time_start_sntp(void);

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
