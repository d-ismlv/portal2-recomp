// Decode i386 va_list explicitly: Darwin ARM64 va_list and C long sizes differ.
#include "rt.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { CPU *cpu; uint64_t cursor; } Arguments;
static uint32_t take32(Arguments *args) {
    if (args->cursor > UINT32_MAX - 3) rt_trap(args->cpu, args->cpu->eip, "variadic argument overflow");
    uint32_t value = R32((uint32_t)args->cursor); args->cursor += 4; return value;
}
static uint64_t take64(Arguments *args) {
    uint64_t lo = take32(args), hi = take32(args);
    return lo | (hi << 32);
}
static uint32_t argument(CPU *c, unsigned i) { return R32(c->esp + 4 + 4 * i); }

static int guest_format(CPU *c, uint32_t destination, uint32_t capacity,
                        uint32_t format, uint32_t argv, FILE *stream) {
    if (!stream) rt_write_span(c, destination, capacity);
    rt_string_length(c, format);
    const char *p = GPTR(format);
    Arguments args = {c, argv};
    uint64_t used = 0;
    #define OUTPUT_VALUE(value) (stream ? fprintf(stream, native, (value)) : snprintf(out, available, native, (value)))
    while (*p) {
        if (*p != '%') {
            if (stream) { if (fputc((unsigned char)*p, stream) == EOF) return -1; }
            else if (used + 1 < capacity) W8(destination + (uint32_t)used, (uint8_t)*p);
            ++used; ++p; continue;
        }
        ++p;
        if (*p == '%') {
            if (stream) { if (fputc('%', stream) == EOF) return -1; }
            else if (used + 1 < capacity) W8(destination + (uint32_t)used, '%');
            ++used; ++p; continue;
        }
        char flags[8] = {0}; size_t flag_count = 0;
        while (*p && strchr("-+ #0", *p)) {
            if (!strchr(flags, *p)) {
                if (flag_count >= sizeof(flags) - 2) rt_trap(c, c->eip, "format flags overflow");
                flags[flag_count++] = *p;
            }
            ++p;
        }
        int width = 0, precision = -1;
        if (*p == '*') {
            width = (int32_t)take32(&args); ++p;
            if (width == INT_MIN) return -1;
            if (width < 0) {
                width = -width;
                if (!strchr(flags, '-')) flags[flag_count++] = '-';
            }
        } else while (isdigit((unsigned char)*p)) {
            if (width > (INT_MAX - (*p - '0')) / 10) return -1;
            width = width * 10 + *p++ - '0';
        }
        if (*p == '.') {
            ++p; precision = 0;
            if (*p == '*') { precision = (int32_t)take32(&args); ++p; if (precision < 0) precision = -1; }
            else while (isdigit((unsigned char)*p)) {
                if (precision > (INT_MAX - (*p - '0')) / 10) return -1;
                precision = precision * 10 + *p++ - '0';
            }
        }
        char length[3] = {0};
        if (*p && strchr("hljztL", *p)) {
            length[0] = *p++;
            if ((length[0] == 'h' || length[0] == 'l') && *p == length[0]) length[1] = *p++;
        }
        char conversion = *p;
        if (!conversion) rt_trap(c, c->eip, "incomplete guest format");
        ++p;
        char native[64], digits[24];
        snprintf(native, sizeof(native), "%%%s", flags);
        if (width) { snprintf(digits, sizeof(digits), "%d", width); strcat(native, digits); }
        if (precision >= 0) { snprintf(digits, sizeof(digits), ".%d", precision); strcat(native, digits); }
        int integer = strchr("diuoxX", conversion) != NULL;
        if (integer) {
            if (length[0] == 'L') rt_trap(c, c->eip, "invalid integer format length");
            strcat(native, "ll"); // all integer values are explicitly widened below
        }
        size_t end = strlen(native); native[end] = conversion; native[end + 1] = 0;
        size_t available = used < capacity ? (size_t)(capacity - used) : 0;
        char *out = available ? GPTR(destination + (uint32_t)used) : NULL;
        int count;
        if (integer) {
            int wide = !strcmp(length, "ll") || !strcmp(length, "j");
            uint64_t value = wide ? take64(&args) : take32(&args);
            if (conversion == 'd' || conversion == 'i') {
                int64_t signed_value = wide ? (int64_t)value : (int32_t)value;
                if (!strcmp(length, "h")) signed_value = (int16_t)value;
                if (!strcmp(length, "hh")) signed_value = (int8_t)value;
                count = OUTPUT_VALUE((long long)signed_value);
            } else {
                if (!strcmp(length, "h")) value = (uint16_t)value;
                if (!strcmp(length, "hh")) value = (uint8_t)value;
                count = OUTPUT_VALUE((unsigned long long)value);
            }
        } else if (conversion == 's' && !length[0]) {
            uint32_t address = take32(&args);
            if (precision < 0) rt_string_length(c, address);
            else for (int i = 0; i < precision; ++i) {
                rt_span(c, address, (uint64_t)i + 1);
                if (!R8(address + (uint32_t)i)) break;
            }
            count = OUTPUT_VALUE((char *)GPTR(address));
        } else if (conversion == 'c' && !length[0]) {
            count = OUTPUT_VALUE((int)take32(&args));
        } else if (conversion == 'p' && !length[0]) {
            count = OUTPUT_VALUE((void *)(uintptr_t)take32(&args));
        } else if (strchr("fFeEgGaA", conversion) && (!length[0] || !strcmp(length, "l"))) {
            uint64_t bits = take64(&args); double value; memcpy(&value, &bits, 8);
            count = OUTPUT_VALUE(value);
        } else if (conversion == 'n' && length[0] != 'L') {
            uint32_t address = take32(&args);
            unsigned size = !strcmp(length, "hh") ? 1 : !strcmp(length, "h") ? 2 :
                            (!strcmp(length, "ll") || !strcmp(length, "j")) ? 8 : 4;
            rt_span(c, address, size);
            if (size == 1) W8(address, (uint32_t)used);
            else if (size == 2) W16(address, (uint32_t)used);
            else if (size == 4) W32(address, (uint32_t)used);
            else W64(address, used);
            count = 0;
        } else rt_trap(c, c->eip, "unsupported guest format conversion");
        if (count < 0) return -1;
        used += (unsigned)count;
        if (used > INT_MAX) return -1;
    }
    #undef OUTPUT_VALUE
    if (!stream && capacity) W8(destination + (uint32_t)(used < capacity ? used : capacity - 1), 0);
    return used <= INT_MAX ? (int)used : -1;
}

int rt_format_shim(CPU *c, const char *name) {
    if (!strcmp(name, "snprintf") || !strcmp(name, "vsnprintf")) {
        uint32_t argv = !strcmp(name, "vsnprintf") ? argument(c, 3) : c->esp + 16;
        c->eax = (uint32_t)guest_format(c, argument(c, 0), argument(c, 1), argument(c, 2), argv, NULL);
        return 1;
    }
    if (!strcmp(name, "printf") || !strcmp(name, "vprintf")) {
        uint32_t argv = !strcmp(name, "vprintf") ? argument(c, 1) : c->esp + 8;
        c->eax = (uint32_t)guest_format(c, 0, 0, argument(c, 0), argv, stdout);
        return 1;
    }
    if (!strcmp(name, "fprintf") || !strcmp(name, "vfprintf")) {
        uint32_t argv = !strcmp(name, "vfprintf") ? argument(c, 2) : c->esp + 12;
        FILE *stream = rt_stream_file(c, argument(c, 0));
        c->eax = (uint32_t)guest_format(c, 0, 0, argument(c, 1), argv, stream);
        return 1;
    }
    return 0;
}
