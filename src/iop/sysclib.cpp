// SYSCLIB: the IOP's C library, and the second, provisional `stdio` table
// that lets modules loading before `STDIO` itself bind to something.
//
// docs/analysis/08-c-library-and-heap.md "The 42 sysclib ordinals" is what
// this follows, including its "Where these deviate from standard C"
// subsection: a retail title's own disc modules import fifteen of these
// ordinals by number, built against exactly the behaviour documented there,
// not against the C standard.

#include "module.hpp"

#include <stdarg.h>
#include <stdint.h>

namespace {

using ps2::module::ExportTable;
using ps2::module::reservedHook;
using ps2::module::slot;

extern "C" {
int _import_loadcore_register(void *table);
}

// ---- ordinals 6, 7, 8, 9: the ctype table ----------------------------------

constexpr uint8_t kUpper = 0x01;
constexpr uint8_t kLower = 0x02;
constexpr uint8_t kDigit = 0x04;
constexpr uint8_t kSpace = 0x08;
constexpr uint8_t kPunct = 0x10;
constexpr uint8_t kControl = 0x20;
constexpr uint8_t kHexLetter = 0x40;

// The classification the reference's own table carries, read off it byte for
// byte rather than derived from what C's `<ctype.h>` would say. Two of these
// are not what a fresh implementation would write: a **digit carries no
// hex-letter flag** (only `A`-`F` and `a`-`f` do), and the space character
// itself is `0x18` -- space *and* punctuation -- where every other whitespace
// character is `0x08` alone. Characters at `0x80` and above are all zero.
constexpr uint8_t classify(int c) {
    uint8_t flags = 0;
    if (c >= 0x09 && c <= 0x0D) {
        flags |= kSpace;
    }
    if (c == ' ') {
        flags |= kSpace | kPunct;
    }
    if ((c >= 0x00 && c <= 0x08) || (c >= 0x0E && c <= 0x1F) || c == 0x7F) {
        flags |= kControl;
    }
    if (c >= '0' && c <= '9') {
        flags |= kDigit;
    }
    if (c >= 'A' && c <= 'F') {
        flags |= kUpper | kHexLetter;
    } else if (c >= 'G' && c <= 'Z') {
        flags |= kUpper;
    }
    if (c >= 'a' && c <= 'f') {
        flags |= kLower | kHexLetter;
    } else if (c >= 'g' && c <= 'z') {
        flags |= kLower;
    }
    if ((c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
        (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E)) {
        flags |= kPunct;
    }
    return flags;
}

// Index i holds the flags for character (i - 1): ordinal 9 exports a pointer
// to &storage[1], so a caller's unmasked `int c` -- including -1 -- lands on
// the right byte without this module doing any offset arithmetic of its own.
struct CtypeStorage {
    uint8_t v[256];
};

constexpr CtypeStorage buildCtypeStorage() {
    CtypeStorage storage{};
    for (int i = 0; i < 256; i++) {
        storage.v[i] = classify(i - 1);
    }
    return storage;
}

constexpr CtypeStorage kCtypeStorage = buildCtypeStorage();

const uint8_t *ctypeTablePtr() {
    return &kCtypeStorage.v[1];
}

// Ordinal 9: the table itself, as a pointer a caller can index directly.
const void *ctypeTable() {
    return ctypeTablePtr();
}

// Ordinal 8: deliberately unmasked ("Where these deviate"), so a caller
// passing something outside [-1, 0xFE] reads whatever static data follows
// the table rather than being clamped into range.
int ctypeLookup(int c) {
    return ctypeTablePtr()[c];
}

// Ordinal 6: only a lowercase letter is folded, by a fixed offset.
int toupperImpl(int c) {
    return (ctypeTablePtr()[c] & kLower) ? c - 0x20 : c;
}

// Ordinal 7: the mirror of ordinal 6.
int tolowerImpl(int c) {
    return (ctypeTablePtr()[c] & kUpper) ? c + 0x20 : c;
}

bool isSpaceChar(int c) {
    return (ctypeTablePtr()[c] & kSpace) != 0;
}

// ---- ordinals 40, 41: the word copier and word filler ----------------------
//
// Both are indexed rather than pointer-incremented, and both are 4-way
// unrolled: not for speed here, but because a plain `while (n--) *d++ = *s++`
// shape is exactly the pattern `-fno-builtin` still lets the loop-idiom
// recognizer fold back into a call to the very memcpy/memset these exist to
// implement, which then fails to link on a freestanding target.

void wordCopyImpl(void *dst, const void *src, uint32_t nbytes) {
    auto *d = static_cast<uint32_t *>(dst);
    const auto *s = static_cast<const uint32_t *>(src);
    uint32_t words = nbytes >> 2;
    uint32_t i = 0;
    for (; i + 4 <= words; i += 4) {
        d[i + 0] = s[i + 0];
        d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 2];
        d[i + 3] = s[i + 3];
    }
    for (; i < words; i++) {
        d[i] = s[i];
    }
}

// Fills with the whole 32-bit `value`, not a byte pattern -- ordinal 14 only
// reaches for this when the fill byte is 0, where the distinction is moot.
void wordFillImpl(void *dst, uint32_t value, uint32_t nbytes) {
    auto *d = static_cast<uint32_t *>(dst);
    uint32_t words = nbytes >> 2;
    uint32_t i = 0;
    for (; i + 4 <= words; i += 4) {
        d[i + 0] = value;
        d[i + 1] = value;
        d[i + 2] = value;
        d[i + 3] = value;
    }
    for (; i < words; i++) {
        d[i] = value;
    }
}

// ---- ordinals 10, 11, 12, 13, 14, 15, 16, 17: mem*/b* -----------------------

void *memchrImpl(const void *s, int c, int n) {
    if (s == nullptr || n <= 0) {
        return nullptr;
    }
    const auto *p = static_cast<const uint8_t *>(s);
    auto target = static_cast<uint8_t>(c);
    for (int i = 0; i < n; i++) {
        if (p[i] == target) {
            return const_cast<uint8_t *>(p + i);
        }
    }
    return nullptr;
}

// Unsigned compare, but the answer is clamped to -1/0/+1 rather than the
// byte difference (unlike strcmp/strncmp, which return the difference).
int memcmpImpl(const void *a, const void *b, int32_t n) {
    const auto *pa = static_cast<const uint8_t *>(a);
    const auto *pb = static_cast<const uint8_t *>(b);
    for (int32_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    return 0;
}

void *memcpyImpl(void *dst, const void *src, uint32_t n) {
    auto d = reinterpret_cast<uintptr_t>(dst);
    auto s = reinterpret_cast<uintptr_t>(src);
    if (((d | s | n) & 3) == 0) {
        wordCopyImpl(dst, src, n);
    } else {
        auto *bd = static_cast<uint8_t *>(dst);
        const auto *bs = static_cast<const uint8_t *>(src);
        for (uint32_t i = 0; i < n; i++) {
            bd[i] = bs[i];
        }
    }
    return dst;
}

void *memmoveImpl(void *dst, const void *src, uint32_t n) {
    auto *d = static_cast<uint8_t *>(dst);
    const auto *s = static_cast<const uint8_t *>(src);
    if (d < s) {
        for (uint32_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (uint32_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

void *memsetImpl(void *s, int c, uint32_t n) {
    auto p = reinterpret_cast<uintptr_t>(s);
    if (c == 0 && ((p | n) & 3) == 0) {
        wordFillImpl(s, 0, n);
    } else {
        auto *b = static_cast<uint8_t *>(s);
        auto v = static_cast<uint8_t>(c);
        for (uint32_t i = 0; i < n; i++) {
            b[i] = v;
        }
    }
    return s;
}

int bcmpImpl(const void *a, const void *b, int32_t n) {
    return memcmpImpl(a, b, n);
}

// BSD argument order: (src, dst, n), unlike memmove's (dst, src, n).
void bcopyImpl(const void *src, void *dst, uint32_t n) {
    memmoveImpl(dst, src, n);
}

void bzeroImpl(void *s, uint32_t n) {
    memsetImpl(s, 0, n);
}

// ---- ordinals 18, 19: prnt and sprintf -------------------------------------

using PrntWriter = void (*)(void *, int);

struct PrintState {
    PrntWriter out;
    void *ctx;
    int total;
};

void emitChar(PrintState &st, int c) {
    st.out(st.ctx, c);
    st.total++;
}

// The stacked arguments are read byte-wise rather than through a `uint32_t *`
// deref, so a misaligned `ap` cannot trap the R3000A the way an unaligned
// `lw` would.
uint32_t readArgWord(const uint8_t *&cursor) {
    uint32_t value = static_cast<uint32_t>(cursor[0]) | (static_cast<uint32_t>(cursor[1]) << 8)
                    | (static_cast<uint32_t>(cursor[2]) << 16)
                    | (static_cast<uint32_t>(cursor[3]) << 24);
    cursor += 4;
    return value;
}

// The shared tail of every integer conversion: sign/space/plus, the `#`
// prefix, zero- or space-padding, and the digits themselves, built least-
// significant first and emitted in reverse.
void emitPaddedNumber(PrintState &st, bool negative, uint32_t magnitude, int base, bool upper,
                       const char *prefix, int width, bool have_width, int prec, bool left,
                       bool zero, bool plus, bool space, bool alt_octal) {
    char digits[32];
    int digit_count = 0;
    if (magnitude == 0) {
        if (prec != 0) {
            digits[digit_count++] = '0';
        }
    } else {
        while (magnitude != 0) {
            auto d = static_cast<int>(magnitude % static_cast<uint32_t>(base));
            digits[digit_count++] = static_cast<char>(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
            magnitude /= static_cast<uint32_t>(base);
        }
    }
    // '#' on octal guarantees at least one leading '0' digit.
    if (alt_octal && (digit_count == 0 || digits[digit_count - 1] != '0')) {
        if (prec < digit_count + 1) {
            prec = digit_count + 1;
        }
    }
    int zero_pad_prec = (prec > digit_count) ? prec - digit_count : 0;

    char sign = 0;
    if (negative) {
        sign = '-';
    } else if (plus) {
        sign = '+';
    } else if (space) {
        sign = ' ';
    }

    int prefix_len = 0;
    while (prefix != nullptr && prefix[prefix_len] != '\0') {
        prefix_len++;
    }

    int content_len = (sign ? 1 : 0) + prefix_len + zero_pad_prec + digit_count;
    int pad = (have_width && width > content_len) ? width - content_len : 0;
    // A precision on an integer conversion makes '0' a no-op, same as '-'.
    bool zero_pad = zero && !left && prec < 0;

    if (!left && !zero_pad) {
        while (pad-- > 0) {
            emitChar(st, ' ');
        }
    }
    if (sign) {
        emitChar(st, sign);
    }
    for (int i = 0; i < prefix_len; i++) {
        emitChar(st, prefix[i]);
    }
    if (!left && zero_pad) {
        while (pad-- > 0) {
            emitChar(st, '0');
        }
    }
    for (int i = 0; i < zero_pad_prec; i++) {
        emitChar(st, '0');
    }
    for (int i = digit_count - 1; i >= 0; i--) {
        emitChar(st, digits[i]);
    }
    if (left) {
        while (pad-- > 0) {
            emitChar(st, ' ');
        }
    }
}

// Ordinal 18: `int prnt(void (*out)(void *ctx, int c), void *ctx, const char
// *fmt, void *ap)`. No floating point; `d i D u U o O` are all treated alike
// since `int` and `long` are the same 32 bits on the IOP (a judgement call --
// the analysis lists them as distinct conversions without saying how the BSD
// long-form ones differ in practice on a 32-bit target).
int prntImpl(PrntWriter out, void *ctx, const char *fmt, void *ap) {
    if (fmt == nullptr) {
        return 0;
    }
    out(ctx, 0x200);
    PrintState st{out, ctx, 0};
    const auto *cursor = static_cast<const uint8_t *>(ap);

    while (*fmt != '\0') {
        if (*fmt != '%') {
            emitChar(st, static_cast<unsigned char>(*fmt));
            fmt++;
            continue;
        }
        fmt++;

        bool left = false, plus = false, space = false, alt = false, zero = false;
        for (;;) {
            if (*fmt == '-') { left = true; fmt++; }
            else if (*fmt == '+') { plus = true; fmt++; }
            else if (*fmt == ' ') { space = true; fmt++; }
            else if (*fmt == '#') { alt = true; fmt++; }
            else if (*fmt == '0') { zero = true; fmt++; }
            else { break; }
        }

        int width = 0;
        bool have_width = false;
        if (*fmt == '*') {
            auto w = static_cast<int32_t>(readArgWord(cursor));
            if (w < 0) { left = true; w = -w; }
            width = w;
            have_width = true;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                have_width = true;
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = static_cast<int32_t>(readArgWord(cursor));
                if (prec < 0) { prec = -1; }
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        // h/l/L: accepted and skipped, with no effect -- int and long are
        // the same width on this target.
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'L') {
            fmt++;
        }

        char conv = *fmt;
        if (conv != '\0') {
            fmt++;
        }

        switch (conv) {
        case 'c': {
            auto ch = static_cast<int32_t>(readArgWord(cursor));
            int pad = have_width ? width - 1 : 0;
            if (!left) { while (pad-- > 0) { emitChar(st, ' '); } }
            emitChar(st, static_cast<unsigned char>(ch));
            if (left) { while (pad-- > 0) { emitChar(st, ' '); } }
            break;
        }
        case 'd': case 'i': case 'D': {
            auto v = static_cast<int32_t>(readArgWord(cursor));
            bool negative = v < 0;
            uint32_t magnitude = negative ? static_cast<uint32_t>(-v) : static_cast<uint32_t>(v);
            emitPaddedNumber(st, negative, magnitude, 10, false, nullptr, width, have_width,
                              prec, left, zero, plus, space, false);
            break;
        }
        case 'u': case 'U': {
            uint32_t v = readArgWord(cursor);
            emitPaddedNumber(st, false, v, 10, false, nullptr, width, have_width, prec, left,
                              zero, false, false, false);
            break;
        }
        case 'o': case 'O': {
            uint32_t v = readArgWord(cursor);
            emitPaddedNumber(st, false, v, 8, false, nullptr, width, have_width, prec, left,
                              zero, false, false, alt);
            break;
        }
        case 'x': {
            uint32_t v = readArgWord(cursor);
            const char *prefix = (alt && v != 0) ? "0x" : nullptr;
            emitPaddedNumber(st, false, v, 16, false, prefix, width, have_width, prec, left,
                              zero, false, false, false);
            break;
        }
        case 'X': {
            uint32_t v = readArgWord(cursor);
            const char *prefix = (alt && v != 0) ? "0X" : nullptr;
            emitPaddedNumber(st, false, v, 16, true, prefix, width, have_width, prec, left,
                              zero, false, false, false);
            break;
        }
        case 'p': {
            uint32_t v = readArgWord(cursor);
            emitPaddedNumber(st, false, v, 16, false, "0x", width, have_width, prec, left, zero,
                              false, false, false);
            break;
        }
        case 's': {
            uint32_t addr = readArgWord(cursor);
            const char *s = reinterpret_cast<const char *>(static_cast<uintptr_t>(addr));
            if (s == nullptr) {
                s = "(null)";
            }
            int len = 0;
            while (s[len] != '\0' && (prec < 0 || len < prec)) {
                len++;
            }
            int pad = have_width ? width - len : 0;
            if (!left) { while (pad-- > 0) { emitChar(st, ' '); } }
            for (int i = 0; i < len; i++) {
                emitChar(st, static_cast<unsigned char>(s[i]));
            }
            if (left) { while (pad-- > 0) { emitChar(st, ' '); } }
            break;
        }
        case 'n': {
            uint32_t addr = readArgWord(cursor);
            auto *out_count = reinterpret_cast<int *>(static_cast<uintptr_t>(addr));
            if (out_count != nullptr) {
                *out_count = st.total;
            }
            break;
        }
        case '\0':
            break;              // a trailing '%' with nothing after it
        default:
            emitChar(st, '%');
            emitChar(st, static_cast<unsigned char>(conv));
            break;
        }
    }

    out(ctx, 0x201);
    return st.total;
}

struct SprintfCursor {
    char *pos;
};

// A character below 0x100 stores and advances; the two out-of-band markers
// (0x200/0x201) store a NUL without advancing, so the buffer is always
// terminated wherever writing stops.
void sprintfWriter(void *ctx, int c) {
    auto *cur = static_cast<SprintfCursor *>(ctx);
    if (c < 0x100) {
        *cur->pos = static_cast<char>(c);
        cur->pos++;
    } else {
        *cur->pos = '\0';
    }
}

// Ordinal 19: `prnt` over the cursor writer above. `ap` on this target's O32
// calling convention is just a pointer walking the stacked arguments, so it
// converts to `void *` without any copying.
int sprintfImpl(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    SprintfCursor cursor{buf};
    int count = prntImpl(sprintfWriter, &cursor, fmt, args);
    va_end(args);
    return count;
}

// ---- ordinals 20-38: the str* family ---------------------------------------

uint32_t strlenImpl(const char *s) {
    if (s == nullptr) {
        return 0;
    }
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

char *strchrImpl(const char *s, int c) {
    if (s == nullptr) {
        return nullptr;
    }
    auto target = static_cast<char>(c);
    for (;;) {
        if (*s == target) {
            return const_cast<char *>(s);
        }
        if (*s == '\0') {
            return nullptr;
        }
        s++;
    }
}

// Ordinal 25: byte-identical to ordinal 21 in the reference.
char *indexImpl(const char *s, int c) {
    return strchrImpl(s, c);
}

char *strrchrImpl(const char *s, int c) {
    if (s == nullptr) {
        return nullptr;
    }
    auto target = static_cast<char>(c);
    const char *found = nullptr;
    for (;;) {
        if (*s == target) {
            found = s;
        }
        if (*s == '\0') {
            break;
        }
        s++;
    }
    return const_cast<char *>(found);
}

// Ordinal 26: byte-identical to ordinal 32 in the reference.
char *rindexImpl(const char *s, int c) {
    return strrchrImpl(s, c);
}

// Signed-char compare (deviation from the standard's unsigned comparison),
// and NULL sorts below any string rather than faulting.
int strcmpImpl(const char *a, const char *b) {
    if (a == nullptr && b == nullptr) {
        return 0;
    }
    if (a == nullptr) {
        return -1;
    }
    if (b == nullptr) {
        return 1;
    }
    for (;;) {
        auto ca = static_cast<signed char>(*a);
        auto cb = static_cast<signed char>(*b);
        if (ca != cb || ca == 0) {
            return static_cast<int>(ca) - static_cast<int>(cb);
        }
        a++;
        b++;
    }
}

int strncmpImpl(const char *a, const char *b, int32_t n) {
    if (a == nullptr && b == nullptr) {
        return 0;
    }
    if (a == nullptr) {
        return -1;
    }
    if (b == nullptr) {
        return 1;
    }
    for (int32_t i = 0; i < n; i++) {
        auto ca = static_cast<signed char>(a[i]);
        auto cb = static_cast<signed char>(b[i]);
        if (ca != cb || ca == 0) {
            return static_cast<int>(ca) - static_cast<int>(cb);
        }
    }
    return 0;
}

char *strcpyImpl(char *dst, const char *src) {
    if (dst == nullptr || src == nullptr) {
        return nullptr;
    }
    char *d = dst;
    while ((*d++ = *src++) != '\0') {}
    return dst;
}

char *strncpyImpl(char *dst, const char *src, uint32_t n) {
    if (dst == nullptr || src == nullptr) {
        return nullptr;
    }
    uint32_t i = 0;
    for (; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

// The alias guard: if the two strings' terminators coincide, the arguments
// are considered the same string end-to-end and nothing is copied.
char *strcatImpl(char *dst, const char *src) {
    if (dst == nullptr || src == nullptr) {
        return nullptr;
    }
    char *dend = dst;
    while (*dend != '\0') { dend++; }
    const char *send = src;
    while (*send != '\0') { send++; }
    if (dend == send) {
        return nullptr;
    }
    char *d = dend;
    while ((*d++ = *src++) != '\0') {}
    return dst;
}

char *strncatImpl(char *dst, const char *src, uint32_t n) {
    char *d = dst;
    while (*d != '\0') { d++; }
    uint32_t i = 0;
    for (; i < n && src[i] != '\0'; i++) {
        d[i] = src[i];
    }
    d[i] = '\0';
    return dst;
}

uint32_t strcspnImpl(const char *s, const char *reject) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        bool found = false;
        for (const char *r = reject; *r != '\0'; r++) {
            if (s[n] == *r) { found = true; break; }
        }
        if (found) {
            break;
        }
        n++;
    }
    return n;
}

uint32_t strspnImpl(const char *s, const char *accept) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        bool found = false;
        for (const char *a = accept; *a != '\0'; a++) {
            if (s[n] == *a) { found = true; break; }
        }
        if (!found) {
            break;
        }
        n++;
    }
    return n;
}

char *strpbrkImpl(const char *s, const char *accept) {
    for (; *s != '\0'; s++) {
        for (const char *a = accept; *a != '\0'; a++) {
            if (*s == *a) {
                return const_cast<char *>(s);
            }
        }
    }
    return nullptr;
}

char *strstrImpl(const char *haystack, const char *needle) {
    if (*needle == '\0') {
        return const_cast<char *>(haystack);
    }
    for (; *haystack != '\0'; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h != '\0' && *n != '\0' && *h == *n) { h++; n++; }
        if (*n == '\0') {
            return const_cast<char *>(haystack);
        }
    }
    return nullptr;
}

// Static state, not reentrant -- exactly as documented (§ ordinal 35).
char *g_strtok_state = nullptr;

bool byteInSet(char c, const char *set) {
    for (; *set != '\0'; set++) {
        if (c == *set) {
            return true;
        }
    }
    return false;
}

char *strtokImpl(char *s, const char *delim) {
    if (s == nullptr) {
        s = g_strtok_state;
    }
    if (s == nullptr) {
        return nullptr;
    }
    while (*s != '\0' && byteInSet(*s, delim)) { s++; }
    if (*s == '\0') {
        g_strtok_state = nullptr;
        return nullptr;
    }
    char *start = s;
    while (*s != '\0' && !byteInSet(*s, delim)) { s++; }
    if (*s != '\0') {
        *s = '\0';
        g_strtok_state = s + 1;
    } else {
        g_strtok_state = nullptr;
    }
    return start;
}

int digitValue(int c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'Z') { return c - 'A' + 10; }
    return -1;
}

// The prefix scan that both strtol and strtoul share: it overrides whatever
// base the caller asked for, unconditionally -- the base is never tested
// before the scan runs.
void applyBasePrefix(const char *&p, int &base) {
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        base = 2;
        p += 2;
    } else if (p[0] == 'o' || p[0] == 'O') {
        base = 8;
        p += 1;
    } else if (base == 0) {
        base = 10;
    }
}

// Ordinal 36: no '+' is accepted at all, and each consecutive '-' toggles
// the sign rather than the usual single optional sign.
int32_t strtolImpl(const char *s, char **endptr, int base) {
    if (s == nullptr) {
        if (endptr != nullptr) { *endptr = nullptr; }
        return 0;
    }
    const char *p = s;
    while (isSpaceChar(static_cast<unsigned char>(*p))) { p++; }
    bool negative = false;
    while (*p == '-') { negative = !negative; p++; }
    applyBasePrefix(p, base);
    uint32_t value = 0;
    bool any = false;
    for (;;) {
        int digit = digitValue(static_cast<unsigned char>(*p));
        if (digit < 0 || digit >= base) {
            break;
        }
        value = value * static_cast<uint32_t>(base) + static_cast<uint32_t>(digit);
        any = true;
        p++;
    }
    if (endptr != nullptr) {
        *endptr = const_cast<char *>(any ? p : s);
    }
    auto result = static_cast<int32_t>(value);
    return negative ? -result : result;
}

// Ordinal 38: unlike strtol, no sign of any kind is accepted.
uint32_t strtoulImpl(const char *s, char **endptr, int base) {
    if (s == nullptr) {
        if (endptr != nullptr) { *endptr = nullptr; }
        return 0;
    }
    const char *p = s;
    while (isSpaceChar(static_cast<unsigned char>(*p))) { p++; }
    applyBasePrefix(p, base);
    uint32_t value = 0;
    bool any = false;
    for (;;) {
        int digit = digitValue(static_cast<unsigned char>(*p));
        if (digit < 0 || digit >= base) {
            break;
        }
        value = value * static_cast<uint32_t>(base) + static_cast<uint32_t>(digit);
        any = true;
        p++;
    }
    if (endptr != nullptr) {
        *endptr = const_cast<char *>(any ? p : s);
    }
    return value;
}

// Ordinal 37: Sony's own, not a standard function -- it returns the end
// pointer, the way `strtol`'s `endptr` output does, instead of the value.
char *atobImpl(char *s, int *v) {
    if (s == nullptr) {
        return nullptr;
    }
    char *end;
    int32_t value = strtolImpl(s, &end, 10);
    if (v != nullptr) {
        *v = value;
    }
    return end;
}

// ---- ordinals 4, 5: setjmp/longjmp -----------------------------------------
//
// These touch the caller's own raw registers, which no C function signature
// can express, so they are hand-written MIPS I rather than compiled from a
// C++ body -- the same reasoning module.hpp gives for the import stubs.
// `int buf[12]`: ra, sp, fp, s0-s7, gp.

extern "C" {
int setjmpImpl(int *buf);
int longjmpImpl(int *buf, int val);
}

asm(".pushsection .text,\"ax\",@progbits\n"
    ".set noreorder\n"
    ".global setjmpImpl\n"
    ".type setjmpImpl, @function\n"
    "setjmpImpl:\n"
    "sw $ra, 0($a0)\n"
    "sw $sp, 4($a0)\n"
    "sw $fp, 8($a0)\n"
    "sw $s0, 12($a0)\n"
    "sw $s1, 16($a0)\n"
    "sw $s2, 20($a0)\n"
    "sw $s3, 24($a0)\n"
    "sw $s4, 28($a0)\n"
    "sw $s5, 32($a0)\n"
    "sw $s6, 36($a0)\n"
    "sw $s7, 40($a0)\n"
    "sw $gp, 44($a0)\n"
    "jr $ra\n"
    "addiu $v0, $zero, 0\n"        // delay slot: setjmp's own return is 0

    ".global longjmpImpl\n"
    ".type longjmpImpl, @function\n"
    "longjmpImpl:\n"
    "lw $s0, 12($a0)\n"
    "lw $s1, 16($a0)\n"
    "lw $s2, 20($a0)\n"
    "lw $s3, 24($a0)\n"
    "lw $s4, 28($a0)\n"
    "lw $s5, 32($a0)\n"
    "lw $s6, 36($a0)\n"
    "lw $s7, 40($a0)\n"
    "lw $gp, 44($a0)\n"
    "lw $sp, 4($a0)\n"
    "lw $fp, 8($a0)\n"
    "lw $ra, 0($a0)\n"
    "nop\n"                        // load-delay: $ra is not ready for jr yet
    "jr $ra\n"
    "move $v0, $a1\n"              // delay slot: $a1 passes through unchanged
    ".set reorder\n"
    ".popsection\n");

// ---- ordinals 2, 3, 39: bare stubs -----------------------------------------

void stub() {}

// ---- ordinals 0, 1: the module entry and (re-)registration ----------------

// Ordinal 1: register both libraries. `stdio_exports` below is already
// written at 1.01, so there is no version to rewrite here the way the
// reference's own entry does it in place -- see the table's own comment for
// why 1.01 and not the reference's stored 1.02.
extern ExportTable<42> sysclib_exports;
extern ExportTable<14> stdio_exports;

int registerLibraries() {
    int sysclib_result = _import_loadcore_register(&sysclib_exports);
    int stdio_result = _import_loadcore_register(&stdio_exports);
    return (sysclib_result < 0 || stdio_result < 0) ? -1 : 0;
}

// Ordinal 0: the module's own entry, re-exported so another module can ask
// for the registration to run again.
int moduleEntry() {
    return registerLibraries();
}

PS2_EXPORT_TABLE ExportTable<42> sysclib_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'s', 'y', 's', 'c', 'l', 'i', 'b', 0},
    {
        slot(moduleEntry),           // 0
        slot(registerLibraries),     // 1
        slot(stub),                  // 2
        slot(stub),                  // 3
        slot(setjmpImpl),            // 4
        slot(longjmpImpl),           // 5
        slot(toupperImpl),           // 6
        slot(tolowerImpl),           // 7
        slot(ctypeLookup),           // 8
        slot(ctypeTable),            // 9
        slot(memchrImpl),            // 10
        slot(memcmpImpl),            // 11
        slot(memcpyImpl),            // 12
        slot(memmoveImpl),           // 13
        slot(memsetImpl),            // 14
        slot(bcmpImpl),              // 15
        slot(bcopyImpl),             // 16
        slot(bzeroImpl),             // 17
        slot(prntImpl),              // 18
        slot(sprintfImpl),           // 19
        slot(strcatImpl),            // 20
        slot(strchrImpl),            // 21
        slot(strcmpImpl),            // 22
        slot(strcpyImpl),            // 23
        slot(strcspnImpl),           // 24
        slot(indexImpl),             // 25
        slot(rindexImpl),            // 26
        slot(strlenImpl),            // 27
        slot(strncatImpl),           // 28
        slot(strncmpImpl),           // 29
        slot(strncpyImpl),           // 30
        slot(strpbrkImpl),           // 31
        slot(strrchrImpl),           // 32
        slot(strspnImpl),            // 33
        slot(strstrImpl),            // 34
        slot(strtokImpl),            // 35
        slot(strtolImpl),            // 36
        slot(atobImpl),              // 37
        slot(strtoulImpl),           // 38
        slot(stub),                  // 39
        slot(wordCopyImpl),          // 40
        slot(wordFillImpl),          // 41
        nullptr,
    },
};

// SYSCLIB's own `stdio` table: 14 entries of `jr $ra`, existing only to be
// superseded once `STDIO` registers its real 1.02 table (docs/analysis/08
// "Superseding"). Registered here at 1.01 -- one below the reference's own
// stored 1.02 -- so that `LOADCORE`'s "strictly greater minor" rule lets
// `STDIO`'s later registration take over rather than being rejected as a
// same-or-lower generation of the same library.
PS2_EXPORT_TABLE ExportTable<14> stdio_exports = {
    ps2::module::kExportMagic,
    0,
    0x0101,
    0,
    {'s', 't', 'd', 'i', 'o', 0, 0, 0},
    {
        slot(reservedHook),          // 0
        slot(reservedHook),          // 1
        slot(reservedHook),          // 2
        slot(reservedHook),          // 3
        slot(reservedHook),          // 4
        slot(reservedHook),          // 5
        slot(reservedHook),          // 6
        slot(reservedHook),          // 7
        slot(reservedHook),          // 8
        slot(reservedHook),          // 9
        slot(reservedHook),          // 10
        slot(reservedHook),          // 11
        slot(reservedHook),          // 12
        slot(reservedHook),          // 13
        nullptr,
    },
};

}  // namespace

PS2_IMPORTS_BEGIN("loadcore", 0x0101)
PS2_IMPORT(_import_loadcore_register, 6)
PS2_IMPORTS_END()

extern "C" {

// The residency code is the registration's sign bit (docs/analysis/11), the
// same convention cdvdman/romdrv/ioman use.
int _module_start(int, char **) {
    return moduleEntry() < 0 ? 1 : 0;
}

}  // extern "C"
