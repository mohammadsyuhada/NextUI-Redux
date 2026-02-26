#ifndef WGET_FETCH_H
#define WGET_FETCH_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Fetch URL content into memory buffer using wget.
 * Drop-in replacement for radio_net_fetch() for non-radio callers.
 *
 * @param url         The URL to fetch
 * @param buffer      Buffer to store response body
 * @param buffer_size Size of buffer
 * @return            Bytes read on success, -1 on failure
 */
int wget_fetch(const char* url, uint8_t* buffer, int buffer_size);

/**
 * Download URL to file with progress reporting and cancellation.
 * Drop-in replacement for http_download_file() using wget.
 * Supports resume via -c flag. Partial files are kept on failure for resume.
 *
 * @param url           The URL to download
 * @param filepath      Local path to save the file
 * @param progress_pct  Optional pointer to receive progress (0-100), can be NULL
 * @param should_stop   Optional pointer to cancellation flag, can be NULL
 * @param speed_bps_out Optional pointer to receive current speed in bytes/sec, can be NULL
 * @param eta_sec_out   Optional pointer to receive estimated time remaining in seconds, can be NULL
 * @return              Bytes downloaded on success, -1 on failure
 */
int wget_download_file(const char* url, const char* filepath,
					   volatile int* progress_pct, volatile bool* should_stop,
					   volatile int* speed_bps_out, volatile int* eta_sec_out);

#endif // WGET_FETCH_H
