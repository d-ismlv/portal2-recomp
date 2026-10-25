// iconv glibc i386 relayé vers l'iconv hôte (libiconv de macOS).
//
// iconv_t invité = handle opaque (rt_handle_*), jamais un pointeur hôte.
// Les arguments char ** / size_t * invités sont relus puis mis à jour après
// la conversion. errno est traduit en valeurs Linux (EILSEQ = 84, pas 92).
// "WCHAR_T" (wchar_t glibc = UCS-4 natif, petit-boutiste sur i386) est
// remplacé par "UCS-4LE" : le WCHAR_T de libiconv suit le wchar_t et la
// locale de l'hôte, pas ceux de l'invité.
//
// SHIMS: iconv_open iconv iconv_close
#include "rt.h"
#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <strings.h>

#define ICONV_KIND 0x49434e56u   // 'ICNV'

static uint32_t arg(CPU *c, unsigned i) { return R32(c->esp + 4 + 4 * i); }
static int linux_iconv_errno(int value) {
    switch (value) {
    case EILSEQ: return 84;
    case E2BIG: return 7;
    case EINVAL: return 22;
    case EBADF: return 9;
    case ENOMEM: return 12;
    default: return value;
    }
}
// Recopie un nom d'encodage en remplaçant WCHAR_T (suffixes //… conservés).
static int encoding(CPU *c, uint32_t address, char *out, size_t size) {
    uint32_t length = rt_string_length(c, address);
    const char *name = GPTR(address);
    if (!strncasecmp(name, "WCHAR_T", 7) && (name[7] == 0 || name[7] == '/'))
        return snprintf(out, size, "UCS-4LE%s", name + 7) < (int)size;
    if (length >= size) return 0;
    memcpy(out, name, (size_t)length + 1);
    return 1;
}

static int iconv_shim(CPU *c, const char *name) {
    if (!strcmp(name, "iconv_open")) {
        char to[128], from[128];
        iconv_t cd = (iconv_t)-1;
        if (encoding(c, arg(c, 0), to, sizeof(to)) && encoding(c, arg(c, 1), from, sizeof(from)))
            cd = iconv_open(to, from);
        else errno = EINVAL;
        if (cd == (iconv_t)-1) { rt_set_errno(c, linux_iconv_errno(errno)); c->eax = UINT32_MAX; return 1; }
        c->eax = rt_handle_new(cd, ICONV_KIND);
        if (!c->eax) { iconv_close(cd); rt_set_errno(c, 12); c->eax = UINT32_MAX; }
    } else if (!strcmp(name, "iconv_close")) {
        uint32_t handle = arg(c, 0);
        if (handle == UINT32_MAX) { rt_set_errno(c, 9); c->eax = UINT32_MAX; return 1; }
        iconv_t cd = rt_handle_free(handle, ICONV_KIND);
        if (!cd) rt_trap(c, c->eip, "iconv_close on unknown iconv_t");
        int result = iconv_close(cd);
        if (result) rt_set_errno(c, linux_iconv_errno(errno));
        c->eax = (uint32_t)result;
    } else if (!strcmp(name, "iconv")) {
        uint32_t handle = arg(c, 0), inbuf = arg(c, 1), inleft = arg(c, 2), outbuf = arg(c, 3), outleft = arg(c, 4);
        if (handle == UINT32_MAX) { rt_set_errno(c, 9); c->eax = UINT32_MAX; return 1; }
        iconv_t cd = rt_handle_get(handle, ICONV_KIND);
        if (!cd) rt_trap(c, c->eip, "iconv on unknown iconv_t");
        uint32_t in = inbuf ? R32(inbuf) : 0, out = outbuf ? R32(outbuf) : 0;
        char *host_in = NULL, *host_out = NULL;
        size_t in_count = 0, out_count = 0, result;
        if (out) { out_count = R32(outleft); host_out = rt_write_span(c, out, out_count); }
        if (in) {
            // Conversion : glibc exige un tampon de sortie (sinon E2BIG immédiat).
            in_count = R32(inleft); host_in = rt_span(c, in, in_count);
            if (!out) rt_trap(c, c->eip, "iconv conversion without output buffer");
            result = iconv(cd, &host_in, &in_count, &host_out, &out_count);
        } else if (out) result = iconv(cd, NULL, NULL, &host_out, &out_count);   // vidage de l'état
        else result = iconv(cd, NULL, NULL, NULL, NULL);                         // réinitialisation
        int saved = errno;
        if (in) { W32(inbuf, in + (uint32_t)(host_in - (char *)GPTR(in))); W32(inleft, (uint32_t)in_count); }
        if (out) { W32(outbuf, out + (uint32_t)(host_out - (char *)GPTR(out))); W32(outleft, (uint32_t)out_count); }
        if (result == (size_t)-1) { rt_set_errno(c, linux_iconv_errno(saved)); c->eax = UINT32_MAX; }
        else c->eax = result > UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)result;
    } else return 0;
    return 1;
}
RT_SHIM_PROVIDER(iconv_shim)
