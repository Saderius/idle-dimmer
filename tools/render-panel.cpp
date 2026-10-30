// Headless renderer for the flyout panel.
//
// This exists because there is no interactive preview in this environment, so the panel's
// appearance can only be checked by generating an image and looking at that. It calls
// PaintPanel() - the same function the real window calls - so the contact sheet exercises
// production drawing code rather than a copy that could silently drift.
//
// Usage: render-panel.exe <output.png> [dpi] [rest|hover|pressed|light|light-hover]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Flyout.h"

// Renders one panel into a 32-bpp top-down DIB and hands back the pixels.
static bool RenderOne(int dpi, const PanelState& state, BYTE** outPixels, int* outW, int* outH) {
    const int w = MulDiv(kPanelWidth, dpi, 96);
    const int h = MulDiv(PanelHeight(state.buttonCount), dpi, 96);

    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = w;
    header.bV5Height = -h;              // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;

    void* pixels = nullptr;
    HBITMAP dib = CreateDIBSection(screen, (BITMAPINFO*)&header, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dib || !pixels) { ReleaseDC(nullptr, screen); return false; }

    memset(pixels, 0, (size_t)w * h * 4);
    HDC dc = CreateCompatibleDC(screen);
    if (!dc) { DeleteObject(dib); ReleaseDC(nullptr, screen); return false; }
    HGDIOBJ old = SelectObject(dc, dib);

    PaintPanel(dc, w, h, state, dpi);

    // Read back with an explicit bitfield layout including the alpha mask. (An earlier version
// requested plain BI_RGB on the theory that GDI reinterpreted the channels; measured, BI_RGB
// and BI_BITFIELDS returns are byte-identical, so that change fixed nothing. The comment
// claimed a mechanism that does not exist and would have misdirected the next reader.)
    // BITMAPV5HEADER, not BITMAPINFO + hand-placed mask DWORDs: sizeof(BITMAPINFO) is 44
    // bytes, so writing four masks at offset 40 ran 8 bytes past the object. It happened to
    // work and was undefined behaviour. V5HEADER is the documented home for an alpha mask.
    // Declare the variable as BITMAPV5HEADER all the way through and pass it straight to
    // GetDIBits, which accepts a BITMAPINFO*. Copying a V5HEADER into a BITMAPINFO with
    // memcpy wrote 124 bytes into a 44-byte object and ran 80 bytes past it, clobbering
    // outPixels, w and h - which is why the composite read garbage while the very same
    // PaintPanel call measured correct in isolation.
    BITMAPV5HEADER v5 = {};
    v5.bV5Size = sizeof(v5);
    v5.bV5Width = w;
    v5.bV5Height = -h;
    v5.bV5Planes = 1;
    v5.bV5BitCount = 32;
    v5.bV5Compression = BI_BITFIELDS;
    v5.bV5SizeImage = (DWORD)((size_t)w * h * 4);
    v5.bV5RedMask = 0x00ff0000;
    v5.bV5GreenMask = 0x0000ff00;
    v5.bV5BlueMask = 0x000000ff;
    v5.bV5AlphaMask = 0xff000000;
    *outPixels = (BYTE*)malloc((size_t)w * h * 4);
    if (!*outPixels) { SelectObject(dc, old); DeleteDC(dc); DeleteObject(dib); ReleaseDC(nullptr, screen); return false; }
    GetDIBits(dc, dib, 0, h, *outPixels, (BITMAPINFO*)&v5, DIB_RGB_COLORS);
    *outW = w; *outH = h;

    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(dib);
    ReleaseDC(nullptr, screen);
    return true;
}

// Minimal PNG writer. The build has no image library and adding one would violate the
// "OS DLLs only" constraint, so this emits the four chunk types a PNG needs: IHDR,
// IDAT (zlib "stored", i.e. uncompressed deflate blocks) and IEND. Uncompressed is fine
// for a build-time contact sheet.
static void Put32(BYTE* p, unsigned v) {
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v;
}
static void Be32(BYTE* p, unsigned v) {
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v;
}

static unsigned Crc32(const BYTE* data, size_t len) {
    static unsigned table[256];
    static bool built = false;
    if (!built) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// A PNG chunk is length(4) + type(4) + data(n) + crc(4), where the CRC covers the type
// and the data but NOT the length. The CRC must be computed after the data is in place,
// so it is written last into the slot that the length computation already reserved.
static void PushChunk(BYTE** buf, size_t* len, const char* type, const BYTE* data, size_t dataLen) {
    const size_t start = *len;
    Be32(*buf + *len, (unsigned)dataLen); (*len) += 4;
    memcpy(*buf + *len, type, 4); (*len) += 4;
    if (dataLen) { memcpy(*buf + *len, data, dataLen); }
    *len += dataLen;
    const unsigned crc = Crc32(*buf + start + 4, 4 + dataLen);
    Be32(*buf + *len, crc); (*len) += 4;
}

static bool WritePng(const char* path, int w, int h, const BYTE* rgba) {
    // Raw scanlines with filter byte 0.
    size_t rawLen = (size_t)h * (1 + (size_t)w * 4);
    BYTE* raw = (BYTE*)malloc(rawLen);
    if (!raw) return false;
    for (int y = 0; y < h; y++) {
        raw[y * (1 + w * 4)] = 0;
        memcpy(raw + y * (1 + w * 4) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }

    // zlib stream: 0x78 0x01, then stored deflate blocks, then adler32.
    size_t blockMax = 65535;
    size_t blocks = (rawLen + blockMax - 1) / blockMax;
    size_t zLen = 2 + (blocks * 5) + rawLen + 4;
    BYTE* z = (BYTE*)malloc(zLen);
    if (!z) { free(raw); return false; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;
    size_t off = 0;
    while (off < rawLen) {
        size_t n = rawLen - off; if (n > blockMax) n = blockMax;
        bool last = (off + n >= rawLen);
        z[zi++] = last ? 1 : 0;
        z[zi++] = (BYTE)(n & 0xFF); z[zi++] = (BYTE)(n >> 8);
        z[zi++] = (BYTE)(~n & 0xFF); z[zi++] = (BYTE)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n); zi += n;
        off += n;
    }
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < rawLen; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    Be32(z + zi, (b << 16) | a); zi += 4;
    free(raw);

    size_t pngLen = 8 + (12 + 13) + (12 + zi) + 12;
    BYTE* png = (BYTE*)malloc(pngLen);
    if (!png) { free(z); return false; }
    size_t p = 0;
    static const BYTE sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    memcpy(png + p, sig, 8); p += 8;

    BYTE ihdr[13];
    Be32(ihdr, (unsigned)w); Be32(ihdr + 4, (unsigned)h);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // colour type RGBA
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    PushChunk(&png, &p, "IHDR", ihdr, sizeof(ihdr));
    PushChunk(&png, &p, "IDAT", z, zi);
    PushChunk(&png, &p, "IEND", nullptr, 0);

    FILE* f = fopen(path, "wb");
    if (!f) { free(png); free(z); return false; }
    fwrite(png, 1, p, f);
    fclose(f);
    free(png); free(z);
    return true;
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "icons/verify-panel.png";

    // The states worth eyeballing: the three that a person actually sees most, plus the
    // disabled one, in both themes, plus dynamic 2-row reflow (5 and 6 buttons).
    struct Case { const char* name; PanelState s; };
    PanelState base = {};
    base.dark = true; base.enabled = true; base.mediaGuard = true;
    base.fullscreenGuard = true; base.autostart = true; base.minutes = 2; base.idleSec = 47; base.design = 3;
    base.buttonCount = 4;
    if (argc > 3) {
        base.hover = strstr(argv[3], "hover") ? hitSliderTrack : hitNone;
        base.pressed = strcmp(argv[3], "pressed") == 0 ? hitSliderTrack : hitNone;
        base.dark = strstr(argv[3], "light") == nullptr;
    }
    // The live accent on this machine, MEASURED 2026-10-24 from AccentPalette[1]: #D4BB8B. Without
    // an accent the fixture's value is 0 (black), and on the dark theme an "on" tile is
    // accent-filled with an onAccent glyph - black on black. Four of the nine cells therefore
    // rendered as empty black boxes with their icons invisible, which is precisely the class of
    // defect the sheet exists to catch, hiding in the sheet itself.
    //
    // This is a fixture, not a measurement: it mirrors the accent so the sheet looks like the app,
    // and it is still no evidence about the live colour. Only the user's screenshot is.
    base.accent = 0xD4BB8B;
    // The matching highlight swatch, so a hovered tile in the sheet paints #ECDCAA the way the
    // live panel does. Fixture values, mirroring AccentPalette[1] and [0] on this machine.
    base.accentHover = 0xECDCAA;

    Case cases[9];
    cases[0] = { "counting",   base };
    cases[1] = { "dimmed",     [&] { PanelState s = base; s.dimmed = true; s.enabled = true; return s; }() };
    cases[2] = { "guards off", [&] { PanelState s = base; s.mediaGuard = false; s.fullscreenGuard = false; return s; }() };
    cases[3] = { "disabled",   [&] { PanelState s = base; s.enabled = false; return s; }() };
    cases[4] = { "light",      [&] { PanelState s = base; s.dark = false; return s; }() };
    cases[5] = { "light dim",  [&] { PanelState s = base; s.dark = false; s.dimmed = true; return s; }() };
    cases[6] = { "5 buttons reflow", [&] { PanelState s = base; s.buttonCount = 5; s.minutes = 30; s.idleSec = 1740; return s; }() };
    cases[7] = { "6 buttons reflow", [&] { PanelState s = base; s.buttonCount = 6; s.autostart = false; return s; }() };
    // The card's populated state. The other eight all have hasDimHistory=false, so they render
    // the em dashes; without this cell the sheet would never show a single real counter and a
    // regression that emptied the card again would be invisible in the one visual check
    // available (AGENTS.md 1: this environment cannot see a real desktop window).
    cases[8] = { "stats", [&] {
        PanelState s = base; s.dimmed = true;
        // One hour up, 22.8 min of it dimmed: the bar fills 38% and the row above reads
        // 0.4 h. Fixture values chosen to be internally consistent, so the sheet cannot
        // show a bar that disagrees with the number it sits under.
        s.wakesFromDim = 7; s.dimmedMs = 1368000; s.sessionDimmedMs = 1368000; s.msSinceDim = 180000;
        s.hasDimHistory = true; s.sessionMs = 3600000;
        s.lastDimKnown = true;
        return s; }() };

    // A single-panel render at an arbitrary DPI. The 3x3 sheet is fixed at 96 DPI for
    // comparison; the user's desktop is 200%, and icon rendering must be checked there
    // rather than inferred from a 1x downsample.
    const int dpi = argc > 2 ? atoi(argv[2]) : 96;
    if (dpi != 96) {
        BYTE* px = nullptr; int w = 0, h = 0;
        if (!RenderOne(dpi, base, &px, &w, &h)) return 2;
        BYTE* rgba = (BYTE*)malloc((size_t)w * h * 4);
        if (!rgba) { free(px); return 2; }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                BYTE* s = px + ((size_t)y * w + x) * 4;
                BYTE* d = rgba + ((size_t)y * w + x) * 4;
                const int a = s[3];
                d[0] = (BYTE)((s[2] * a + 32 * (255 - a)) / 255);
                d[1] = (BYTE)((s[1] * a + 34 * (255 - a)) / 255);
                d[2] = (BYTE)((s[0] * a + 40 * (255 - a)) / 255);
                d[3] = 255;
            }
        }
        free(px);
        const bool ok = WritePng(out, w, h, rgba);
        free(rgba);
        if (!ok) { fprintf(stderr, "png write failed\n"); return 2; }
        printf("wrote %s (%dx%d at %d DPI)\n", out, w, h, dpi);
        return 0;
    }

    const int cellW = 420, cellH = 430, pad = 12, cols = 3;
    int rows = 3;
    const int sheetW = pad + cols * (cellW + pad);
    const int sheetH = pad + rows * (cellH + pad);

    BYTE* sheet = (BYTE*)calloc((size_t)sheetW * sheetH * 4, 1);
    if (!sheet) return 2;

    for (int i = 0; i < 9; i++) {
        BYTE* px = nullptr; int w = 0, h = 0;
        if (!RenderOne(96, cases[i].s, &px, &w, &h)) { fprintf(stderr, "render %d failed\n", i); return 2; }
        int cx = pad + (i % cols) * (cellW + pad);
        int cy = pad + (i / cols) * (cellH + pad);
        // Composite over a neutral desktop grey, since the real panel is translucent.
        // Win32 DIB memory layout is BGRA: s[0]=Blue, s[1]=Green, s[2]=Red, s[3]=Alpha.
        // The PNG writer expects RGBA: d[0]=Red, d[1]=Green, d[2]=Blue, d[3]=Alpha.
        for (int y = 0; y < h && y < cellH; y++) {
            for (int x = 0; x < w && x < cellW; x++) {
                BYTE* s = px + ((size_t)y * w + x) * 4;
                int a = s[3];
                int r = (s[2] * a + 32 * (255 - a)) / 255;
                int g = (s[1] * a + 34 * (255 - a)) / 255;
                int b = (s[0] * a + 40 * (255 - a)) / 255;
                BYTE* d = sheet + ((size_t)(cy + y) * sheetW + (cx + x)) * 4;
                d[0] = (BYTE)r; d[1] = (BYTE)g; d[2] = (BYTE)b; d[3] = 255;
            }
        }
        free(px);
    }

    bool ok = WritePng(out, sheetW, sheetH, sheet);
    free(sheet);
    if (!ok) { fprintf(stderr, "png write failed\n"); return 2; }
    printf("wrote %s (%dx%d)\n", out, sheetW, sheetH);
    return 0;
}
