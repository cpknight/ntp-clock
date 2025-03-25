#include "ntp_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

/* NTP protocol definitions */
#define NTP_PORT 123                  /* Default NTP port */
#define NTP_TIMESTAMP_DELTA 2208988800UL  /* Seconds between 1900 (NTP epoch) and 1970 (Unix epoch) */
#define NTP_VERSION 4                 /* NTP version 4 */
#define NTP_MODE_CLIENT 3             /* NTP client mode */
#define NTP_STRATUM_MAX 16            /* Maximum stratum value */
#define NTP_TIMEOUT_SEC 5             /* Default timeout in seconds */

/* NTP packet structure */
typedef struct {
    uint8_t li_vn_mode;           /* Leap indicator, version and mode */
    uint8_t stratum;              /* Stratum level */
    uint8_t poll;                 /* Poll interval */
    uint8_t precision;            /* Precision */
    uint32_t root_delay;          /* Root delay */
    uint32_t root_dispersion;     /* Root dispersion */
    uint32_t ref_id;              /* Reference ID */
    uint32_t ref_timestamp_sec;   /* Reference timestamp seconds */
    uint32_t ref_timestamp_frac;  /* Reference timestamp fraction */
    uint32_t orig_timestamp_sec;  /* Origin timestamp seconds */
    uint32_t orig_timestamp_frac; /* Origin timestamp fraction */
    uint32_t recv_timestamp_sec;  /* Receive timestamp seconds */
    uint32_t recv_timestamp_frac; /* Receive timestamp fraction */
    uint32_t tx_timestamp_sec;    /* Transmit timestamp seconds */
    uint32_t tx_timestamp_frac;   /* Transmit timestamp fraction */
} ntp_packet_t;

/* NTP client state */
typedef struct {
    bool initialized;             /* Whether the client is initialized */
    bool ever_synced;             /* Whether the client has ever synced */
    ntp_config_t config;          /* Client configuration */
    time_t last_sync_time;        /* Last successful sync time (Unix timestamp) */
    time_t ntp_time;              /* Last retrieved NTP time (Unix timestamp) */
    int64_t time_offset;          /* Offset between system time and NTP time in seconds */
    pthread_mutex_t lock;         /* Mutex for thread safety */
} ntp_client_state_t;

/* Global client state */
static ntp_client_state_t client_state = {
    .initialized = false,
    .ever_synced = false,
    .last_sync_time = 0,
    .ntp_time = 0,
    .time_offset = 0
};

/**
 * @brief Convert from NTP time format to Unix time format
 */
static time_t ntp_time_to_unix_time(uint32_t ntp_seconds) {
    return ntp_seconds - NTP_TIMESTAMP_DELTA;
}

/**
 * @brief Convert from Unix time format to NTP time format
 */
static uint32_t unix_time_to_ntp_time(time_t unix_seconds) {
    return unix_seconds + NTP_TIMESTAMP_DELTA;
}

/**
 * @brief Get current time as NTP timestamp fraction (0..2^32-1)
 */
static uint32_t get_fraction_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* Convert microseconds to a fraction of a second (2^32 units) */
    return (uint32_t)((double)tv.tv_usec * 4294.967296);
}

/**
 * @brief Create and initialize an NTP packet for sending to the server
 */
static void create_ntp_packet(ntp_packet_t *packet) {
    struct timeval tv;
    
    /* Initialize the packet with zeros */
    memset(packet, 0, sizeof(ntp_packet_t));
    
    /* Set leap indicator, version, and mode */
    packet->li_vn_mode = (0 << 6) | (NTP_VERSION << 3) | NTP_MODE_CLIENT;
    
    /* Set transmit timestamp */
    gettimeofday(&tv, NULL);
    packet->tx_timestamp_sec = htonl(unix_time_to_ntp_time(tv.tv_sec));
    packet->tx_timestamp_frac = htonl(get_fraction_time());
}

/**
 * @brief Resolve a hostname to an IP address
 * 
 * @param hostname Hostname to resolve
 * @param ip_str Buffer to store the IP address string
 * @param ip_len Length of the IP address buffer
 * @return true if successful, false otherwise
 */
static bool resolve_hostname(const char *hostname, char *ip_str, size_t ip_len) {
    struct addrinfo hints, *res;
    int status;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    
    if ((status = getaddrinfo(hostname, NULL, &hints, &res)) != 0) {
        return false;
    }
    
    /* Convert the first result to a string */
    void *addr;
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
    addr = &(ipv4->sin_addr);
    
    if (inet_ntop(res->ai_family, addr, ip_str, ip_len) == NULL) {
        freeaddrinfo(res);
        return false;
    }
    
    freeaddrinfo(res);
    return true;
}

/**
 * @brief Send an NTP request to a server and wait for a response
 * 
 * @param server_name NTP server hostname or IP address
 * @param server_port NTP server port
 * @param timeout_ms Timeout in milliseconds
 * @param response Pointer to store the NTP response
 * @return ntp_status_t Status code
 */
static ntp_status_t send_ntp_request(const char *server_name, uint16_t server_port, 
                                    uint32_t timeout_ms, ntp_packet_t *response) {
    int sockfd;
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    ntp_packet_t packet;
    char ip_str[INET_ADDRSTRLEN];
    fd_set readfds;
    struct timeval timeout;
    int select_result;
    
    /* Create UDP socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        return NTP_ERROR_NETWORK;
    }
    
    /* Set socket timeout */
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        close(sockfd);
        return NTP_ERROR_NETWORK;
    }
    
    /* Resolve hostname to IP address */
    if (!resolve_hostname(server_name, ip_str, sizeof(ip_str))) {
        close(sockfd);
        return NTP_ERROR_NETWORK;
    }
    
    /* Prepare server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    
    if (inet_pton(AF_INET, ip_str, &server_addr.sin_addr) <= 0) {
        close(sockfd);
        return NTP_ERROR_NETWORK;
    }
    
    /* Create and send the NTP packet */
    create_ntp_packet(&packet);
    
    if (sendto(sockfd, &packet, sizeof(packet), 0, 
              (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return NTP_ERROR_NETWORK;
    }
    
    /* Wait for response with timeout */
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    
    select_result = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
    
    if (select_result < 0) {
        close(sockfd);
        return NTP_ERROR_NETWORK;
    } else if (select_result == 0) {
        close(sockfd);
        return NTP_ERROR_TIMEOUT;
    }
    
    /* Receive the response */
    if (recvfrom(sockfd, response, sizeof(ntp_packet_t), 0,
                (struct sockaddr*)&server_addr, &addr_len) < 0) {
        close(sockfd);
        return NTP_ERROR_NETWORK;
    }
    
    /* Convert network byte order to host byte order */
    response->recv_timestamp_sec = ntohl(response->recv_timestamp_sec);
    response->recv_timestamp_frac = ntohl(response->recv_timestamp_frac);
    response->tx_timestamp_sec = ntohl(response->tx_timestamp_sec);
    response->tx_timestamp_frac = ntohl(response->tx_timestamp_frac);
    
    close(sockfd);
    return NTP_OK;
}

ntp_status_t ntp_init(const ntp_config_t *config) {
    if (config == NULL) {
        return NTP_ERROR_INVALID_PARAM;
    }
    
    /* Initialize mutex */
    if (pthread_mutex_init(&client_state.lock, NULL) != 0) {
        return NTP_ERROR_NETWORK;
    }
    
    pthread_mutex_lock(&client_state.lock);
    
    /* Copy configuration */
    memcpy(&client_state.config, config, sizeof(ntp_config_t));
    
    /* Initialize state */
    client_state.initialized = true;
    client_state.ever_synced = false;
    client_state.last_sync_time = 0;
    client_state.ntp_time = 0;
    client_state.time_offset = 0;
    
    pthread_mutex_unlock(&client_state.lock);
    
    return NTP_OK;
}

void ntp_cleanup(void) {
    pthread_mutex_lock(&client_state.lock);
    
    client_state.initialized = false;
    
    pthread_mutex_unlock(&client_state.lock);
    pthread_mutex_destroy(&client_state.lock);
}

ntp_status_t ntp_sync(void) {
    ntp_packet_t response;
    ntp_status_t status;
    time_t server_time, current_time;
    struct timeval local_time;
    uint32_t attempts = 0;
    
    pthread_mutex_lock(&client_state.lock);
    
    if (!client_state.initialized) {
        pthread_mutex_unlock(&client_state.lock);
        return NTP_ERROR_NOT_INIT;
    }
    
    /* Try to sync with server, with retries */
    do {
        status = send_ntp_request(
            client_state.config.server_name,
            client_state.config.server_port,
            client_state.config.timeout_ms,
            &response
        );
        
        attempts++;
        
        /* If failed and we have retries left, sleep a bit and try again */
        if (status != NTP_OK && attempts < client_state.config.retry_count) {
            pthread_mutex_unlock(&client_state.lock);
            usleep(500000); /* 500ms */
            pthread_mutex_lock(&client_state.lock);
        }
    } while (status != NTP_OK && attempts < client_state.config.retry_count);
    
    if (status != NTP_OK) {
        pthread_mutex_unlock(&client_state.lock);
        return status;
    }
    
    /* Validate the server's response */
    if ((response.li_vn_mode & 0x07) != 4 /* Server mode */ &&
        (response.li_vn_mode & 0x07) != 2 /* Symmetric passive mode */) {
        pthread_mutex_unlock(&client_state.lock);
        return NTP_ERROR_SERVER;
    }
    
    if (response.stratum == 0 || response.stratum >= NTP_STRATUM_MAX) {
        pthread_mutex_unlock(&client_state.lock);
        return NTP_ERROR_SERVER;
    }
    
    /* Get server time from response */
    server_time = ntp_time_to_unix_time(response.tx_timestamp_sec);
    
    /* Get current system time */
    gettimeofday(&local_time, NULL);
    current_time = local_time.tv_sec;
    
    /* Calculate time offset */
    client_state.time_offset = server_time - current_time;
    
    /* Update state */
    client_state.last_sync_time = current_time;
    client_state.ntp_time = server_time;
    client_state.ever_synced = true;
    
    pthread_mutex_unlock(&client_state.lock);
    
    return NTP_OK;
}

time_t ntp_getCurrentTime(void) 
{
    struct timeval current_time;
    time_t adjusted_time;
    
    pthread_mutex_lock(&client_state.lock);
    
    if (!client_state.initialized || !client_state.ever_synced) {
        pthread_mutex_unlock(&client_state.lock);
        return 0;
    }
    
    /* Get current system time */
    gettimeofday(&current_time, NULL);
    
    /* Apply the offset to get NTP-adjusted time */
    adjusted_time = current_time.tv_sec + client_state.time_offset;
    
    pthread_mutex_unlock(&client_state.lock);
    
    return adjusted_time;
}

int64_t ntp_getTimeSinceLastSync(void) 
{
    struct timeval current_time;
    int64_t time_since_sync;
    
    pthread_mutex_lock(&client_state.lock);
    
    if (!client_state.initialized || !client_state.ever_synced) {
        pthread_mutex_unlock(&client_state.lock);
        return -1;
    }
    
    /* Get current system time */
    gettimeofday(&current_time, NULL);
    
    /* Calculate time since last sync */
    time_since_sync = current_time.tv_sec - client_state.last_sync_time;
    
    pthread_mutex_unlock(&client_state.lock);
    
    return time_since_sync;
}

bool ntp_getServerName(char *buffer, size_t buffer_size) 
{
    if (buffer == NULL || buffer_size == 0) {
        return false;
    }
    
    pthread_mutex_lock(&client_state.lock);
    
    if (!client_state.initialized || !client_state.ever_synced) {
        pthread_mutex_unlock(&client_state.lock);
        return false;
    }
    
    /* Copy server name to the provided buffer */
    strncpy(buffer, client_state.config.server_name, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';  /* Ensure null termination */
    
    pthread_mutex_unlock(&client_state.lock);
    
    return true;
}

bool ntp_hasEverSynced(void) {
    bool synced;
    
    pthread_mutex_lock(&client_state.lock);
    synced = client_state.initialized && client_state.ever_synced;
    pthread_mutex_unlock(&client_state.lock);
    
    return synced;
}

ntp_status_t ntp_setServer(const char *server_name) {
    if (server_name == NULL) {
        return NTP_ERROR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&client_state.lock);
    
    if (!client_state.initialized) {
        pthread_mutex_unlock(&client_state.lock);
        return NTP_ERROR_NOT_INIT;
    }
    
    /* Update server name in configuration */
    strncpy(client_state.config.server_name, server_name, 
            sizeof(client_state.config.server_name) - 1);
    client_state.config.server_name[sizeof(client_state.config.server_name) - 1] = '\0';
    
    pthread_mutex_unlock(&client_state.lock);
    
    return NTP_OK;
}

double ntp_getCurrentTimeWithMicros(void) {
    struct timeval current_time;
    double adjusted_time;
    
    pthread_mutex_lock(&client_state.lock);
    
    if (!client_state.initialized || !client_state.ever_synced) {
        pthread_mutex_unlock(&client_state.lock);
        return 0.0;
    }
    
    /* Get current system time with microsecond precision */
    gettimeofday(&current_time, NULL);
    
    /* Apply the offset to get NTP-adjusted time with microsecond precision */
    adjusted_time = (double)current_time.tv_sec + (double)current_time.tv_usec / 1000000.0;
    adjusted_time += (double)client_state.time_offset;
    
    pthread_mutex_unlock(&client_state.lock);
    
    return adjusted_time;
}

int ntp_getCurrentHundredths(void) {
    double time_with_micros;
    double fractional_part;
    int hundredths;
    
    /* Get the current time with microsecond precision */
    time_with_micros = ntp_getCurrentTimeWithMicros();
    
    if (time_with_micros == 0.0) {
        return 0;
    }
    
    /* Extract just the fractional part */
    fractional_part = time_with_micros - (double)(long)time_with_micros;
    
    /* Convert to hundredths (0-99) */
    hundredths = (int)(fractional_part * 100.0) % 100;
    
    return hundredths;
}
