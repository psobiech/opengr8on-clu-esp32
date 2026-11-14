/**
 * @file clu_tftp.h
 * @brief TFTP server for CLU drives (a:\\, p:\\). Port of src/vclu/TFTPServer.ts (GET_AND_REPLACE mode).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the TFTP task (serving disabled).
 * @param root VFS directory holding drive subdirs ("/fs" -> /fs/a, /fs/p).
 */
void clu_tftp_init(const char *root);

/** @brief Serving stops after this many seconds without traffic. */
#define CLU_TFTP_IDLE_S     300
/** @brief Max size of an uploaded file. */
#define CLU_TFTP_MAX_UPLOAD (2u * 1024 * 1024)

/**
 * @brief Enable / disable serving (req_start_ftp / req_tftp_stop).
 *
 * A transfer in progress completes. TFTP has no authentication: while enabled only @p peer_ip
 * (the host that sent the authenticated req_start_ftp) is served, and serving stops after
 * CLU_TFTP_IDLE_S without traffic.
 *
 * @param on      Enable serving.
 * @param peer_ip Allowed peer, IPv4 in network byte order.
 */
void clu_tftp_enable(bool on, uint32_t peer_ip);

/** @brief Whether serving is enabled. */
bool clu_tftp_enabled(void);

/**
 * @brief Upload notification, called after each successful WRQ.
 * @param path Uploaded VFS path (may be NULL).
 */
typedef void (*clu_tftp_upload_cb_t)(const char *path);

/** @brief Set the upload callback. */
void clu_tftp_set_upload_cb(clu_tftp_upload_cb_t upload_callback);

#ifdef __cplusplus
}
#endif
