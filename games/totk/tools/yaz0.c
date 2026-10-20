// Yaz0 decoder for scripts/prepare_shader_seed.py (ctypes): BOTW's RomFS packs (.pack members, .sbfsha, .sesetlist, .ssarc)
// are Yaz0, which Python does not read; a Python loop would take hours over the 15 GB RomFS, this about half a minute.
// Built by build_app.py (step_shaders) into build/libyaz0.dylib; not part of the app.
#include <stddef.h>
#include <stdint.h>

// Decodes src (a whole Yaz0 file, 16-byte header included) into dst (its size from the header). Returns the bytes
// written, or -1 when the stream ends early or a back-reference points outside what was written.
long yaz0_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t size) {
    size_t i = 16, w = 0;
    while (w < size) {
        if (i >= n) return -1;
        uint8_t flags = src[i++];
        for (int bit = 0; bit < 8 && w < size; bit++) {
            if (flags & (0x80 >> bit)) {
                if (i >= n) return -1;
                dst[w++] = src[i++];
                continue;
            }
            if (i + 1 >= n) return -1;
            unsigned a = src[i], b = src[i + 1];
            i += 2;
            size_t dist = ((a & 0xf) << 8 | b) + 1, len = a >> 4;
            if (!len) {
                if (i >= n) return -1;
                len = src[i++] + 0x12;
            } else {
                len += 2;
            }
            if (dist > w || len > size - w) return -1;
            for (size_t k = 0; k < len; k++, w++) dst[w] = dst[w - dist];
        }
    }
    return (long)w;
}
