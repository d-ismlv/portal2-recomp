// Sockets BSD avec les constantes et structures Linux i386. Les descripteurs
// sont ceux de l'hôte (comme open) ; sockaddr Linux {u16 famille, ...} est
// traduit vers sockaddr Darwin {u8 len, u8 famille, ...}. fd_set Linux i386 et
// Darwin ont la même disposition (32 mots de 32 bits, bit fd%32 du mot fd/32).
// Les appels bloquants libèrent le verrou d'exécution invité.
// SHIMS: socket bind listen accept connect send recv sendto recvfrom select poll
// SHIMS: setsockopt getsockname gethostbyname inet_addr
#include "posix.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static uint32_t arg(CPU *c, unsigned i) { return R32(c->esp + 4 + 4 * i); }
static void finish(CPU *c, long result) { if (result < 0) rt_save_errno(c); c->eax = (uint32_t)result; }
static void fail(CPU *c, int linux_errno) { rt_set_errno(c, linux_errno); c->eax = UINT32_MAX; }

enum { L_AF_UNSPEC = 0, L_AF_UNIX = 1, L_AF_INET = 2, L_AF_INET6 = 10 };

// sockaddr invité Linux -> sockaddr hôte. Renvoie un errno Linux, 0 si succès.
static int host_address(CPU *c, uint32_t address, uint32_t length, struct sockaddr_storage *out, socklen_t *out_length) {
    if (!address || length < 2 || length > 128) return 22;
    const uint8_t *in = rt_span(c, address, length);
    uint16_t family; memcpy(&family, in, 2);
    memset(out, 0, sizeof(*out));
    switch (family) {
    case L_AF_INET: {
        if (length < 16) return 22;
        struct sockaddr_in *v4 = (struct sockaddr_in *)out;
        v4->sin_len = sizeof(*v4); v4->sin_family = AF_INET;
        memcpy(&v4->sin_port, in + 2, 2); memcpy(&v4->sin_addr, in + 4, 4);
        *out_length = sizeof(*v4); return 0;
    }
    case L_AF_INET6: {
        if (length < 24) return 22;
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)out;
        v6->sin6_len = sizeof(*v6); v6->sin6_family = AF_INET6;
        memcpy(&v6->sin6_port, in + 2, 2); memcpy(&v6->sin6_flowinfo, in + 4, 4);
        memcpy(&v6->sin6_addr, in + 8, 16);
        if (length >= 28) memcpy(&v6->sin6_scope_id, in + 24, 4);
        *out_length = sizeof(*v6); return 0;
    }
    case L_AF_UNIX: {
        struct sockaddr_un *un = (struct sockaddr_un *)out;
        size_t path = length - 2;
        if (path && in[2] == 0) rt_trap(c, c->eip, "Linux abstract AF_UNIX socket names are not supported");
        size_t used = strnlen((const char *)in + 2, path);
        if (used >= sizeof(un->sun_path)) return 36;
        un->sun_family = AF_UNIX; memcpy(un->sun_path, in + 2, used);
        un->sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) + used + 1);
        *out_length = un->sun_len; return 0;
    }
    case L_AF_UNSPEC:
        out->ss_len = 2; out->ss_family = AF_UNSPEC; *out_length = 2; return 0;
    default: return 97; // EAFNOSUPPORT
    }
}

// sockaddr hôte -> invité, avec la sémantique Linux de troncature : la longueur
// réelle est rendue même si le tampon invité est plus petit.
static void guest_address(CPU *c, const struct sockaddr_storage *in, socklen_t in_length, uint32_t address, uint32_t length_address) {
    if (!address) return;
    uint8_t out[128] = {0}; uint32_t size;
    switch (in->ss_family) {
    case AF_INET: {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)in;
        uint16_t family = L_AF_INET; memcpy(out, &family, 2);
        memcpy(out + 2, &v4->sin_port, 2); memcpy(out + 4, &v4->sin_addr, 4); size = 16; break;
    }
    case AF_INET6: {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)in;
        uint16_t family = L_AF_INET6; memcpy(out, &family, 2);
        memcpy(out + 2, &v6->sin6_port, 2); memcpy(out + 4, &v6->sin6_flowinfo, 4);
        memcpy(out + 8, &v6->sin6_addr, 16); memcpy(out + 24, &v6->sin6_scope_id, 4); size = 28; break;
    }
    case AF_UNIX: {
        const struct sockaddr_un *un = (const struct sockaddr_un *)in;
        uint16_t family = L_AF_UNIX; memcpy(out, &family, 2);
        size_t path = in_length > offsetof(struct sockaddr_un, sun_path) ? strnlen(un->sun_path, in_length - offsetof(struct sockaddr_un, sun_path)) : 0;
        memcpy(out + 2, un->sun_path, path); size = (uint32_t)(2 + path + (path ? 1 : 0)); break;
    }
    case AF_UNSPEC: size = 2; break;
    default: rt_trap(c, c->eip, "host returned an untranslated socket address family");
    }
    rt_write_span(c, length_address, 4);
    uint32_t capacity = R32(length_address);
    if ((int32_t)capacity < 0) capacity = 0;
    uint32_t copy = capacity < size ? capacity : size;
    if (copy) memcpy(rt_write_span(c, address, copy), out, copy);
    W32(length_address, size);
}

static int host_message_flags(CPU *c, uint32_t flags, int receiving) {
    int native = 0;
    if (flags & 0x1) native |= MSG_OOB;
    if (flags & 0x2) native |= MSG_PEEK;
    if (flags & 0x4) native |= MSG_DONTROUTE;
    if (flags & 0x40) native |= MSG_DONTWAIT;
    if (flags & 0x80) native |= MSG_EOR;
    if (flags & 0x100) native |= MSG_WAITALL;
    if (flags & 0x4000) native |= MSG_NOSIGNAL;
    // MSG_CONFIRM n'est qu'une indication ARP sans effet observable.
    uint32_t known = 0x1 | 0x2 | 0x4 | 0x40 | 0x80 | 0x100 | 0x4000 | (receiving ? 0 : 0x800);
    if (flags & ~known) rt_trap(c, c->eip, "socket message flags not translated (MSG_TRUNC/MSG_MORE/...)");
    return native;
}

// Les opérations réseau bloquantes libèrent le verrou invité.
#define BLOCKING(call) ({ unsigned saved_ = rt_guest_suspend(); __typeof__(call) result_ = (call); \
    int error_ = errno; rt_guest_resume(saved_); errno = error_; result_; })

static int socket_option(CPU *c, int fd, int level, int name, uint32_t value, uint32_t length) {
    const void *data = length ? rt_span(c, value, length) : NULL;
    int integer = length >= 4 ? (int32_t)R32(value) : 0, native_level, native_name;
    switch (level) {
    case 1: // SOL_SOCKET
        native_level = SOL_SOCKET;
        switch (name) {
        case 2: native_name = SO_REUSEADDR; break; case 5: native_name = SO_DONTROUTE; break;
        case 6: native_name = SO_BROADCAST; break; case 7: native_name = SO_SNDBUF; break;
        case 8: native_name = SO_RCVBUF; break; case 9: native_name = SO_KEEPALIVE; break;
        case 10: native_name = SO_OOBINLINE; break; case 15: native_name = SO_REUSEPORT; break;
        case 18: native_name = SO_RCVLOWAT; break; case 19: native_name = SO_SNDLOWAT; break;
        case 13: { // SO_LINGER : struct linger {int,int} identique
            if (length < 8) { errno = EINVAL; return -1; }
            struct linger linger = {(int32_t)R32(value), (int32_t)R32(value + 4)};
            return setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
        }
        case 20: case 21: { // SO_RCVTIMEO/SO_SNDTIMEO : timeval i386 {long,long}
            if (length < 8) { errno = EINVAL; return -1; }
            struct timeval tv = {(int32_t)R32(value), (int32_t)R32(value + 4)};
            return setsockopt(fd, SOL_SOCKET, name == 20 ? SO_RCVTIMEO : SO_SNDTIMEO, &tv, sizeof(tv));
        }
        case 3: case 4: errno = ENOPROTOOPT; return -1; // SO_TYPE/SO_ERROR : lecture seule
        default: rt_trap(c, c->eip, "SOL_SOCKET option not translated");
        }
        break;
    case 0: // IPPROTO_IP
        native_level = IPPROTO_IP;
        switch (name) {
        case 1: native_name = IP_TOS; break;
        case 2: native_name = IP_TTL; break;
        case 33: case 34: { // IP_MULTICAST_TTL/LOOP : int ou octet sous Linux, u_char sous Darwin
            if (!length) { errno = EINVAL; return -1; }
            unsigned char byte = (unsigned char)(length >= 4 ? integer : R8(value));
            return setsockopt(fd, IPPROTO_IP, name == 33 ? IP_MULTICAST_TTL : IP_MULTICAST_LOOP, &byte, 1);
        }
        case 32: { // IP_MULTICAST_IF : in_addr, ip_mreq ou ip_mreqn
            if (length < 4) { errno = EINVAL; return -1; }
            struct in_addr address; memcpy(&address, rt_span(c, length >= 8 ? value + 4 : value, 4), 4);
            if (length >= 12 && R32(value + 8) && !address.s_addr) rt_trap(c, c->eip, "ip_mreqn interface index not translated");
            return setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &address, sizeof(address));
        }
        case 35: case 36: { // IP_ADD/DROP_MEMBERSHIP : ip_mreq (8) ou ip_mreqn (12)
            if (length < 8) { errno = EINVAL; return -1; }
            struct ip_mreq request; memcpy(&request, rt_span(c, value, 8), 8);
            if (length >= 12 && R32(value + 8) && !request.imr_interface.s_addr) rt_trap(c, c->eip, "ip_mreqn interface index not translated");
            return setsockopt(fd, IPPROTO_IP, name == 35 ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &request, sizeof(request));
        }
        default: rt_trap(c, c->eip, "IPPROTO_IP option not translated");
        }
        break;
    case 6: // IPPROTO_TCP
        native_level = IPPROTO_TCP;
        switch (name) {
        case 1: native_name = TCP_NODELAY; break; case 4: native_name = TCP_KEEPALIVE; break;
        case 5: native_name = TCP_KEEPINTVL; break; case 6: native_name = TCP_KEEPCNT; break;
        default: rt_trap(c, c->eip, "IPPROTO_TCP option not translated");
        }
        break;
    case 41: // IPPROTO_IPV6
        native_level = IPPROTO_IPV6;
        if (name == 26) native_name = IPV6_V6ONLY;
        else rt_trap(c, c->eip, "IPPROTO_IPV6 option not translated");
        break;
    default: rt_trap(c, c->eip, "socket option level not translated");
    }
    if (length < 4) { errno = EINVAL; return -1; }
    (void)data;
    return setsockopt(fd, native_level, native_name, &integer, sizeof(integer));
}

static uint32_t hostent_buffer, hostent_size;
static int lookup_host(CPU *c, const char *name) {
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_CANONNAME;
    int error = BLOCKING(getaddrinfo(name, NULL, &hints, &list));
    if (error) return -1; // h_errno n'est pas exposé (pas d'import __h_errno_location)
    size_t count = 0;
    for (struct addrinfo *i = list; i; i = i->ai_next) if (i->ai_family == AF_INET) ++count;
    const char *canonical = list && list->ai_canonname ? list->ai_canonname : name;
    size_t name_size = strlen(canonical) + 1;
    // struct hostent i386 : h_name, h_aliases, h_addrtype, h_length, h_addr_list (20 octets),
    // puis aliases[1], addr_list[count+1], adresses et nom.
    uint64_t total = 20 + 4 + 4 * ((uint64_t)count + 1) + 4 * (uint64_t)count + name_size;
    if (total > hostent_size) {
        uint32_t grown = total > UINT32_MAX ? 0 : rt_realloc(hostent_buffer, (uint32_t)total);
        if (!grown) { freeaddrinfo(list); rt_set_errno(c, 12); return -1; }
        hostent_buffer = grown; hostent_size = (uint32_t)total;
    }
    uint32_t base = hostent_buffer, aliases = base + 20, addresses = aliases + 4;
    uint32_t data = addresses + 4 * ((uint32_t)count + 1), text = data + 4 * (uint32_t)count;
    W32(aliases, 0);
    size_t index = 0;
    for (struct addrinfo *i = list; i; i = i->ai_next) {
        if (i->ai_family != AF_INET) continue;
        memcpy(GPTR(data + 4 * (uint32_t)index), &((struct sockaddr_in *)i->ai_addr)->sin_addr, 4);
        W32(addresses + 4 * (uint32_t)index, data + 4 * (uint32_t)index); ++index;
    }
    W32(addresses + 4 * (uint32_t)count, 0);
    memcpy(GPTR(text), canonical, name_size);
    freeaddrinfo(list);
    W32(base, text); W32(base + 4, aliases); W32(base + 8, L_AF_INET); W32(base + 12, 4); W32(base + 16, addresses);
    return 0;
}

static short host_poll_events(uint32_t events) {
    short native = 0;
    if (events & 0x1) native |= POLLIN;
    if (events & 0x2) native |= POLLPRI;
    if (events & 0x4) native |= POLLOUT;
    if (events & 0x40) native |= POLLRDNORM;
    if (events & 0x80) native |= POLLRDBAND;
    if (events & 0x100) native |= POLLWRNORM;
    if (events & 0x200) native |= POLLWRBAND;
    return native;
}
static uint16_t linux_poll_events(short native) {
    uint16_t events = 0;
    if (native & POLLIN) events |= 0x1;
    if (native & POLLPRI) events |= 0x2;
    if (native & POLLOUT) events |= 0x4;
    if (native & POLLERR) events |= 0x8;
    if (native & POLLHUP) events |= 0x10;
    if (native & POLLNVAL) events |= 0x20;
    if (native & POLLRDNORM) events |= 0x40;
    if (native & POLLRDBAND) events |= 0x80;
    if (native & POLLWRNORM) events |= 0x100;
    if (native & POLLWRBAND) events |= 0x200;
    return events;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

static int net_shim(CPU *c, const char *name) {
    struct sockaddr_storage address; socklen_t length; int error;
    if (!strcmp(name, "socket")) {
        int domain = (int32_t)arg(c, 0), type = (int32_t)arg(c, 1), protocol = (int32_t)arg(c, 2);
        int native_domain = domain == L_AF_INET ? AF_INET : domain == L_AF_INET6 ? AF_INET6 : domain == L_AF_UNIX ? AF_UNIX : -1;
        if (native_domain < 0) { fail(c, 97); return 1; }
        int kind = type & 0xf, flags = type & ~0xf;
        if (flags & ~(0x800 | 0x80000) || kind < 1 || kind > 5 || kind == 4) { fail(c, 22); return 1; }
        int fd = socket(native_domain, kind, protocol);
        if (fd >= 0 && (flags & 0x800) && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)) { error = errno; close(fd); errno = error; fd = -1; }
        if (fd >= 0 && (flags & 0x80000) && fcntl(fd, F_SETFD, FD_CLOEXEC)) { error = errno; close(fd); errno = error; fd = -1; }
        finish(c, fd);
    } else if (!strcmp(name, "bind") || !strcmp(name, "connect")) {
        int fd = (int32_t)arg(c, 0);
        if ((error = host_address(c, arg(c, 1), arg(c, 2), &address, &length))) { fail(c, error); return 1; }
        int result = !strcmp(name, "bind") ? bind(fd, (struct sockaddr *)&address, length)
                                           : BLOCKING(connect(fd, (struct sockaddr *)&address, length));
        finish(c, result);
    } else if (!strcmp(name, "listen")) {
        finish(c, listen((int32_t)arg(c, 0), (int32_t)arg(c, 1)));
    } else if (!strcmp(name, "accept")) {
        int fd = (int32_t)arg(c, 0); uint32_t out = arg(c, 1), out_length = arg(c, 2);
        length = sizeof(address);
        int result = BLOCKING(accept(fd, (struct sockaddr *)&address, &length));
        if (result >= 0 && out) guest_address(c, &address, length, out, out_length);
        finish(c, result);
    } else if (!strcmp(name, "getsockname")) {
        length = sizeof(address);
        int result = getsockname((int32_t)arg(c, 0), (struct sockaddr *)&address, &length);
        if (!result) guest_address(c, &address, length, arg(c, 1), arg(c, 2));
        finish(c, result);
    } else if (!strcmp(name, "send") || !strcmp(name, "sendto")) {
        int fd = (int32_t)arg(c, 0); uint32_t buffer = arg(c, 1), size = arg(c, 2);
        int flags = host_message_flags(c, arg(c, 3), 0);
        const void *data = rt_span(c, buffer, size);
        uint32_t target = !strcmp(name, "sendto") ? arg(c, 4) : 0;
        if (target && (error = host_address(c, target, arg(c, 5), &address, &length))) { fail(c, error); return 1; }
        ssize_t result = BLOCKING(target ? sendto(fd, data, size, flags, (struct sockaddr *)&address, length)
                                         : send(fd, data, size, flags));
        finish(c, result);
    } else if (!strcmp(name, "recv") || !strcmp(name, "recvfrom")) {
        int fd = (int32_t)arg(c, 0); uint32_t buffer = arg(c, 1), size = arg(c, 2);
        int flags = host_message_flags(c, arg(c, 3), 1);
        void *data = rt_write_span(c, buffer, size);
        uint32_t source = !strcmp(name, "recvfrom") ? arg(c, 4) : 0, source_length = source ? arg(c, 5) : 0;
        length = sizeof(address); memset(&address, 0, sizeof(address));
        ssize_t result = BLOCKING(recvfrom(fd, data, size, flags, source ? (struct sockaddr *)&address : NULL, source ? &length : NULL));
        if (result >= 0 && source) {
            if (length) guest_address(c, &address, length, source, source_length);
            else { rt_write_span(c, source_length, 4); W32(source_length, 0); }
        }
        finish(c, result);
    } else if (!strcmp(name, "setsockopt")) {
        finish(c, socket_option(c, (int32_t)arg(c, 0), (int32_t)arg(c, 1), (int32_t)arg(c, 2), arg(c, 3), arg(c, 4)));
    } else if (!strcmp(name, "select")) {
        int count = (int32_t)arg(c, 0); uint32_t sets[3] = {arg(c, 1), arg(c, 2), arg(c, 3)}, timeout = arg(c, 4);
        if (count < 0 || count > FD_SETSIZE) { fail(c, 22); return 1; }
        fd_set native[3]; fd_set *pointers[3]; size_t bytes = (size_t)((count + 31) / 32) * 4;
        for (int i = 0; i < 3; ++i) {
            pointers[i] = sets[i] ? &native[i] : NULL;
            if (!sets[i]) continue;
            FD_ZERO(&native[i]);
            memcpy(&native[i], rt_write_span(c, sets[i], bytes), bytes);
        }
        struct timeval tv, *wait = NULL; double start = now(), limit = 0;
        if (timeout) {
            rt_write_span(c, timeout, 8);
            int32_t seconds = (int32_t)R32(timeout), micro = (int32_t)R32(timeout + 4);
            if (seconds < 0 || micro < 0 || micro >= 1000000) { fail(c, 22); return 1; }
            tv.tv_sec = seconds; tv.tv_usec = micro; wait = &tv; limit = seconds + micro * 1e-6;
        }
        int result = BLOCKING(select(count, pointers[0], pointers[1], pointers[2], wait));
        error = errno;
        if (timeout) { // Linux écrit le temps restant
            double left = limit - (now() - start);
            if (left < 0 || result == 0) left = 0;
            uint32_t seconds = (uint32_t)left, micro = (uint32_t)((left - seconds) * 1e6);
            if (micro > 999999) micro = 999999;
            W32(timeout, seconds); W32(timeout + 4, micro);
        }
        if (result >= 0) for (int i = 0; i < 3; ++i) if (sets[i]) memcpy(GPTR(sets[i]), &native[i], bytes);
        errno = error; finish(c, result);
    } else if (!strcmp(name, "poll")) {
        uint32_t fds = arg(c, 0), count = arg(c, 1); int timeout = (int32_t)arg(c, 2);
        if (count > 65536) { fail(c, 22); return 1; }
        struct pollfd *native = calloc(count ? count : 1, sizeof(*native));
        if (!native) { fail(c, 12); return 1; }
        rt_write_span(c, fds, (uint64_t)count * 8);
        for (uint32_t i = 0; i < count; ++i) {
            native[i].fd = (int32_t)R32(fds + 8 * i);
            native[i].events = host_poll_events(R16(fds + 8 * i + 4));
        }
        int result = BLOCKING(poll(native, count, timeout));
        if (result >= 0) for (uint32_t i = 0; i < count; ++i) W16(fds + 8 * i + 6, linux_poll_events(native[i].revents));
        error = errno; free(native); errno = error;
        finish(c, result);
    } else if (!strcmp(name, "gethostbyname")) {
        uint32_t text = arg(c, 0); rt_string_length(c, text);
        c->eax = lookup_host(c, GPTR(text)) ? 0 : hostent_buffer;
    } else if (!strcmp(name, "inet_addr")) {
        uint32_t text = arg(c, 0); rt_string_length(c, text);
        c->eax = inet_addr(GPTR(text));
    } else return 0;
    return 1;
}
RT_SHIM_PROVIDER(net_shim)
