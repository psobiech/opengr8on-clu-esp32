/**
 * @file clu_status_display.h
 * @brief Status screen on the board's OLED (CLU_DISPLAY): serial number, IP addresses, date and
 * time, TF-Bus modules online, uptime.
 */
#pragma once

/**
 * @brief Set up the display and start the task that refreshes it every second.
 *
 * A missing display is logged and no task is started. Call at boot, after net_start() (the IP rows
 * stay "-" until an interface has an address).
 */
void clu_status_display_start(void);
