#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/**
 * @brief NTP client configuration structure
 */
typedef struct {
    char server_name[256];    /* NTP server hostname or IP address */
    uint16_t server_port;     /* NTP server port, typically 123 */
    uint32_t timeout_ms;      /* Timeout for NTP requests in milliseconds */
    uint32_t retry_count;     /* Number of retries for failed requests */
    uint32_t sync_interval;   /* Time between automatic syncs in seconds */
} ntp_config_t;

/**
 * @brief NTP client status codes
 */
typedef enum {
    NTP_OK = 0,              /* Operation completed successfully */
    NTP_ERROR_NETWORK,       /* Network error */
    NTP_ERROR_TIMEOUT,       /* Request timed out */
    NTP_ERROR_SERVER,        /* Server error response */
    NTP_ERROR_INVALID_PARAM, /* Invalid parameter */
    NTP_ERROR_NOT_INIT       /* Client not initialized */
} ntp_status_t;

/**
 * @brief Initialize the NTP client with the given configuration
 * 
 * @param config Pointer to NTP client configuration
 * @return ntp_status_t Status code indicating success or error
 */
ntp_status_t ntp_init(const ntp_config_t *config);

/**
 * @brief Clean up resources used by the NTP client
 */
void ntp_cleanup(void);

/**
 * @brief Synchronize time with the configured NTP server
 * 
 * This function blocks until synchronization completes or fails.
 * 
 * @return ntp_status_t Status code indicating success or error
 */
ntp_status_t ntp_sync(void);

/**
 * @brief Get the current time in seconds since the epoch (UTC)
 * 
 * This returns the current time based on the last successful sync with
 * the NTP server, adjusted by the local system clock.
 * 
 * @return time_t Current time in seconds since the epoch, or 0 on error
 */
time_t ntp_getCurrentTime(void);

/**
 * @brief Get the number of seconds since the last successful NTP sync
 * 
 * @return int64_t Seconds since last sync, or -1 if never synced
 */
int64_t ntp_getTimeSinceLastSync(void);

/**
 * @brief Get the name of the NTP server used for the last successful sync
 * 
 * @param buffer Buffer to store the server name
 * @param buffer_size Size of the buffer
 * @return bool true if a server name was retrieved, false otherwise
 */
bool ntp_getServerName(char *buffer, size_t buffer_size);

/**
 * @brief Check if the NTP client has ever successfully synced
 * 
 * @return bool true if at least one sync has succeeded, false otherwise
 */
bool ntp_hasEverSynced(void);

/**
 * @brief Set a new NTP server to use for future sync operations
 * 
 * @param server_name NTP server hostname or IP address
 * @return ntp_status_t Status code indicating success or error
 */
ntp_status_t ntp_setServer(const char *server_name);

/**
 * @brief Get the current time with microsecond precision in seconds since the epoch (UTC)
 *
 * This returns the current time based on the last successful sync with
 * the NTP server, adjusted by the local system clock, with microsecond precision.
 *
 * @return double Current time in seconds since the epoch with microsecond precision, or 0 on error
 */
double ntp_getCurrentTimeWithMicros(void);

/**
 * @brief Get the current hundredths of a second (0-99)
 *
 * This returns just the hundredths portion of the current NTP-adjusted time.
 *
 * @return int Current hundredths of a second (0-99), or 0 on error
 */
int ntp_getCurrentHundredths(void);

#endif /* NTP_CLIENT_H */

