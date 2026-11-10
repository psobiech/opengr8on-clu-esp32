/**
 * @file clu_diag.h
 * @brief OM diagnostics pack (req_diagnostic_refresh).
 */
#pragma once

/**
 * @brief Write m:\\DIAGNOSTIC_PACK.JSON, which OM then downloads over TFTP.
 *
 * @param ip Local IP address reported in the pack.
 * @return 0 on success, or the CLU error code OM knows:
 *         1 = "Error while creating m:\ partition",
 *         2 = "Error while saving the file on the m:\ partition".
 */
int clu_diag_write(const char *ip);
