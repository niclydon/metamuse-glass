#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define AF_BLUETOOTH_VAL 31
#define ATT_CID_VAL      4
#define BDADDR_LE_PUBLIC 1

struct sockaddr_l2_compat {
    sa_family_t l2_family;
    uint16_t    l2_psm;
    uint8_t     l2_bdaddr[6];
    uint16_t    l2_cid;
    uint8_t     l2_bdaddr_type;
};

static void fixup_l2_addr(struct sockaddr *addr, socklen_t *addrlen) {
    if (!addr || !addrlen || *addrlen < 12) return;
    struct sockaddr_l2_compat *l2 = (struct sockaddr_l2_compat *)addr;
    if (l2->l2_family == AF_BLUETOOTH_VAL && l2->l2_cid == ATT_CID_VAL) {
        if (*addrlen < sizeof(struct sockaddr_l2_compat)) {
            *addrlen = sizeof(struct sockaddr_l2_compat);
        }
        if (l2->l2_bdaddr_type == 0) {
            l2->l2_bdaddr_type = BDADDR_LE_PUBLIC;
        }
    }
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    static int (*real_getsockname)(int, struct sockaddr *, socklen_t *) = NULL;
    if (!real_getsockname) real_getsockname = dlsym(RTLD_NEXT, "getsockname");
    int rc = real_getsockname(sockfd, addr, addrlen);
    if (rc == 0) fixup_l2_addr(addr, addrlen);
    return rc;
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    static int (*real_getpeername)(int, struct sockaddr *, socklen_t *) = NULL;
    if (!real_getpeername) real_getpeername = dlsym(RTLD_NEXT, "getpeername");
    int rc = real_getpeername(sockfd, addr, addrlen);
    if (rc == 0) fixup_l2_addr(addr, addrlen);
    return rc;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    static int (*real_accept)(int, struct sockaddr *, socklen_t *) = NULL;
    if (!real_accept) real_accept = dlsym(RTLD_NEXT, "accept");
    int rc = real_accept(sockfd, addr, addrlen);
    if (rc >= 0) fixup_l2_addr(addr, addrlen);
    return rc;
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    static int (*real_accept4)(int, struct sockaddr *, socklen_t *, int) = NULL;
    if (!real_accept4) real_accept4 = dlsym(RTLD_NEXT, "accept4");
    int rc = real_accept4(sockfd, addr, addrlen, flags);
    if (rc >= 0) fixup_l2_addr(addr, addrlen);
    return rc;
}
