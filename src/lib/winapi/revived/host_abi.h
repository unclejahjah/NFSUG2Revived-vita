/* host_abi.h -- the mailboxes the C side shares with the "page": on the Vita, revived_link.cpp. */
#ifndef NFSU2_VITA_REVIVED_HOST_ABI_H
#define NFSU2_VITA_REVIVED_HOST_ABI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { HOST_KIND_NET = 4, HOST_KIND_LAN = 7 };
void nfsu2_register(int kind, void *ptr, uint32_t bytes);
#ifdef __cplusplus
}
#endif
#endif
