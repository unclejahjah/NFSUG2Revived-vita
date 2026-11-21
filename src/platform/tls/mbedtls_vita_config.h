/* mbedtls_vita_config.h -- Mbed TLS on the Vita (MBEDTLS_USER_CONFIG_FILE, on top of the library's default config):
 * a TLS client for the Revived server's WebSocket (src/platform/tls.cpp). No files, no BSD sockets of its own, no
 * timing module: tls.cpp gives it a SceNet socket. Entropy comes from the kernel's generator
 * (sceKernelGetRandomNumber, tls.cpp mbedtls_hardware_poll), and its clock is the platform one
 * (tls.cpp mbedtls_ms_time). */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT   /* mbedtls_ms_time: tls.cpp */
/* client only */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_PROTO_DTLS
/* records up to 16 KB each way */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 16384
