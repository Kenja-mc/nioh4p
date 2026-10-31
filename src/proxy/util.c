/* nioh3p core: util (split from nioh3p_proxy.c) */
#include "nioh3p_internal.h"

/* ------------------------------------------------------- freestanding libc */


int stricmp_a(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return (unsigned char)ca - (unsigned char)cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strrchr_a(const char *s, int c)
{
    const char *last = 0;
    for (; *s; s++)
        if (*s == (char)c)
            last = s;
    return (char *)last;
}

char *strstr_a(const char *h, const char *n)
{
    if (!*n)
        return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b)
            return (char *)h;
    }
    return 0;
}

char *strcat_a(char *d, const char *s)
{
    char *p = d + strlen(d);
    while ((*p++ = *s++))
        ;
    return d;
}


/* minimal vsnprintf: %[0][width][.prec][l|ll|z](d|i|u|x|X|s|p|c|%) */
int vfmt(char *dst, int cap, const char *f, va_list ap)
{
    int n = 0;
    char tmp[32];
    while (*f && n < cap - 1) {
        if (*f != '%') {
            dst[n++] = *f++;
            continue;
        }
        f++;
        int zero = 0, width = 0, prec = -1, alt = 0, left = 0;
        for (;; f++) {
            if (*f == '0') { zero = 1; continue; }
            if (*f == '#') { alt = 1; continue; }
            if (*f == '-') { left = 1; continue; }
            if (*f == '+' || *f == ' ') continue;
            break;
        }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        int longs = 0;
        while (*f == 'l' || *f == 'z') { longs += (*f == 'l') ? 1 : 2; f++; }
        char spec = *f ? *f++ : 0;
        int len = 0;
        if (spec == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (n < cap - 1 && *s && (prec < 0 || len < prec)) {
                dst[n++] = *s++;
                len++;
            }
            continue;
        } else if (spec == 'f') {
            double v = va_arg(ap, double);
            if (v < 0) { tmp[len++] = '-'; v = -v; }
            if (prec < 0) prec = 6;
            unsigned long long ip = (unsigned long long)v;
            double frac = v - (double)ip;
            char id[24];
            int idn = 0;
            do { id[idn++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
            while (idn) tmp[len++] = id[--idn];
            if (prec > 0) {
                tmp[len++] = '.';
                for (int k = 0; k < prec && len < (int)sizeof(tmp) - 2; k++) {
                    frac *= 10.0;
                    int dg = (int)frac;
                    if (dg > 9) dg = 9;
                    tmp[len++] = (char)('0' + dg);
                    frac -= (double)dg;
                }
            }
        } else if (spec == 'c') {
            tmp[len++] = (char)va_arg(ap, int);
        } else if (spec == '%') {
            tmp[len++] = '%';
        } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'p') {
            unsigned long long v;
            int neg = 0;
            if (spec == 'p') {
                v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
                spec = 'x';
            } else if (spec == 'd' || spec == 'i') {
                /* Fetch with the argument's own signed type. Read as unsigned,
                 * a negative int is already a large positive number before any
                 * sign test runs (role -1 was logged as 4294967295). Negating
                 * in unsigned arithmetic keeps INT_MIN/LLONG_MIN defined. */
                long long sv = longs >= 2 ? va_arg(ap, long long)
                             : longs == 1 ? va_arg(ap, long)
                             : va_arg(ap, int);
                neg = sv < 0;
                v = neg ? 0ull - (unsigned long long)sv : (unsigned long long)sv;
            } else if (longs >= 2) {
                v = va_arg(ap, unsigned long long);
            } else if (longs == 1) {
                v = va_arg(ap, unsigned long);
            } else {
                v = (unsigned long long)va_arg(ap, unsigned int);
            }
            char digits[24];
            int dn = 0;
            unsigned base = (spec == 'd' || spec == 'i' || spec == 'u') ? 10 : 16;
            const char *hex = (spec == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            do {
                digits[dn++] = hex[v % base];
                v /= base;
            } while (v);
            if (neg)
                digits[dn++] = '-';
            if (alt && base == 16) {
                digits[dn++] = (spec == 'X') ? 'X' : 'x';
                digits[dn++] = '0';
            }
            if (!left)
                while (width-- > dn)
                    tmp[len++] = zero ? '0' : ' ';
            while (dn)
                tmp[len++] = digits[--dn];
            if (left)
                while (width-- > dn)
                    tmp[len++] = ' ';
        } else {
            tmp[len++] = '%';
            if (spec)
                tmp[len++] = spec;
        }
        for (int i = 0; i < len && n < cap - 1; i++)
            dst[n++] = tmp[i];
    }
    /* cap <= 0 must not write at all: cap - 1 would be dst[-1], and the
     * callers that pass cap - n do reach a negative cap. */
    if (cap > 0)
        dst[n < cap ? n : cap - 1] = 0;
    return n;
}

int fmt(char *dst, int cap, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vfmt(dst, cap, f, ap);
    va_end(ap);
    return n;
}
