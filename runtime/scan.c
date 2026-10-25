#include "rt.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static void put_integer(CPU *c, uint32_t address, const char *length, uint64_t value) {
    unsigned size = !strcmp(length, "hh") ? 1 : !strcmp(length, "h") ? 2 :
                    (!strcmp(length, "ll") || !strcmp(length, "j")) ? 8 : 4;
    rt_span(c, address, size);
    if (size == 1) W8(address, (uint32_t)value);
    else if (size == 2) W16(address, (uint32_t)value);
    else if (size == 4) W32(address, (uint32_t)value);
    else W64(address, value);
}
static int guest_scan(CPU *c, uint32_t source, uint32_t format, uint32_t argv) {
    rt_string_length(c, source); rt_string_length(c, format);
    const char *start = GPTR(source), *input = start, *p = GPTR(format);
    unsigned assigned = 0;
    while (*p) {
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*p)) ++p;
            while (isspace((unsigned char)*input)) ++input;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            char literal = *p++;
            if (literal == '%') ++p;
            if (*input != literal) return !*input && !assigned ? EOF : (int)assigned;
            ++input; continue;
        }
        ++p;
        int suppressed = *p == '*'; if (suppressed) ++p;
        char width[16] = {0}; size_t digits = 0;
        while (isdigit((unsigned char)*p)) {
            if (digits == sizeof(width) - 1) rt_trap(c, c->eip, "scanf field width overflow");
            width[digits++] = *p++;
        }
        char length[3] = {0};
        if (*p && strchr("hljztL", *p)) {
            length[0] = *p++;
            if ((length[0] == 'h' || length[0] == 'l') && *p == length[0]) length[1] = *p++;
        }
        char conversion = *p;
        if (!conversion) rt_trap(c, c->eip, "incomplete guest scanf conversion");
        ++p;
        uint32_t address = 0;
        if (!suppressed) { rt_span(c, argv, 4); address = R32(argv); argv += 4; }
        if (conversion == 'n') {
            if (!suppressed) put_integer(c, address, length, (uint64_t)(input - start));
            continue;
        }
        char native[256];
        int position = snprintf(native, sizeof(native), "%%%s%s", suppressed ? "*" : "", width);
        if (strchr("diuoxX", conversion)) {
            if (length[0] == 'L') rt_trap(c, c->eip, "unsupported scanf integer length");
            strcpy(native + position, "ll"); position += 2;
        } else if (strchr("aAeEfFgG", conversion)) {
            if (length[0] && strcmp(length, "l")) rt_trap(c, c->eip, "unsupported scanf floating length");
            strcpy(native + position, "l"); ++position;
        } else if (!strchr("sc[", conversion) || length[0]) {
            rt_trap(c, c->eip, "unsupported guest scanf conversion");
        }
        native[position++] = conversion;
        if (conversion == '[') {
            if (*p == '^') native[position++] = *p++;
            if (*p == ']') native[position++] = *p++;
            while (*p && *p != ']') {
                if (position >= (int)sizeof(native) - 5) rt_trap(c, c->eip, "scanf scanset overflow");
                native[position++] = *p++;
            }
            if (*p != ']') rt_trap(c, c->eip, "unterminated scanf scanset");
            native[position++] = *p++;
        }
        strcpy(native + position, "%n");
        int consumed = -1, result;
        if (suppressed) result = sscanf(input, native, &consumed);
        else if (conversion == 'd' || conversion == 'i') {
            long long value = 0;
            result = sscanf(input, native, &value, &consumed);
            if (result == 1) put_integer(c, address, length, (uint64_t)value);
        } else if (strchr("uoxX", conversion)) {
            unsigned long long value = 0;
            result = sscanf(input, native, &value, &consumed);
            if (result == 1) put_integer(c, address, length, value);
        } else if (strchr("aAeEfFgG", conversion)) {
            double value = 0;
            result = sscanf(input, native, &value, &consumed);
            if (result == 1) {
                rt_span(c, address, length[0] ? 8 : 4);
                if (length[0]) WD(address, value); else WF(address, (float)value);
            }
        } else {
            size_t remaining = strlen(input), capacity = remaining + 1;
            char *value = calloc(1, capacity);
            if (!value) rt_trap(c, c->eip, "cannot allocate scanf buffer");
            result = sscanf(input, native, value, &consumed);
            if (result == 1) {
                size_t size = conversion == 'c' ? (size_t)consumed : strlen(value) + 1;
                memcpy(rt_write_span(c, address, size), value, size);
            }
            free(value);
        }
        if (consumed < 0) return result == EOF && !assigned ? EOF : (int)assigned;
        if (!suppressed) ++assigned;
        input += consumed;
    }
    return (int)assigned;
}
int rt_scan_shim(CPU *c, const char *name) {
    if (strcmp(name, "__isoc99_sscanf") && strcmp(name, "sscanf")) return 0;
    c->eax = (uint32_t)guest_scan(c, R32(c->esp + 4), R32(c->esp + 8), c->esp + 12);
    return 1;
}
