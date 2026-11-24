/* SPDX-License-Identifier: AGPL-3.0-only
 * Vintage Chonk: desktop renderer and benchmark for the MFE engine.
 *
 *   mfe_render PRESET.mfp|init SCORE.txt|demo OUT.wav     render to a 16-bit stereo WAV
 *   mfe_render PRESET.mfp|init --bench [voices]            CPU cost per voice on this PC
 *
 * Score: one event per line, "<ms> on <note> <vel>", "<ms> off <note>", "<ms> cc <n> <v>",
 * "<ms> bend <-8192..8191>", "<ms> end". Lines starting with # are comments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mfe.h"

typedef struct { long ms; char op[8]; int a, b; } ev_t;

static const char *DEMO =
    "0 on 48 100\n0 on 55 90\n0 on 60 90\n0 on 64 80\n1500 off 48\n1500 off 55\n1500 off 60\n1500 off 64\n"
    "1700 on 36 120\n1950 off 36\n2000 on 43 110\n2250 off 43\n2300 on 48 120\n2550 off 48\n"
    "2600 cc 1 127\n2600 on 72 100\n4000 off 72\n4000 cc 1 0\n5500 end\n";

static int load_preset(const char *path, mfe_patch_t *p)
{
    if (!strcmp(path, "init")) { mfe_patch_init(p); return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    uint8_t buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int r = mfe_preset_read(p, buf, n);
    if (r) fprintf(stderr, "%s: preset error %d\n", path, r);
    return r;
}

static int parse_score(const char *text, ev_t *ev, int cap)
{
    int n = 0;
    const char *s = text;
    while (*s && n < cap) {
        char line[128];
        size_t l = strcspn(s, "\n");
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, s, l);
        line[l] = 0;
        s += l + (s[l] == '\n');
        if (line[0] == '#' || !line[0]) continue;
        ev_t e = {0};
        if (sscanf(line, "%ld %7s %d %d", &e.ms, e.op, &e.a, &e.b) >= 2) ev[n++] = e;
    }
    return n;
}

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }

static int render(mfe_engine_t *e, const char *score_path, const char *out)
{
    char *text = 0;
    if (!strcmp(score_path, "demo")) text = (char *)DEMO;
    else {
        FILE *f = fopen(score_path, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", score_path); return 1; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        text = calloc(1, (size_t)n + 1);
        if (fread(text, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }
        fclose(f);
    }
    static ev_t ev[4096];
    int nev = parse_score(text, ev, 4096);
    long end_ms = 0;
    for (int i = 0; i < nev; i++) if (ev[i].ms > end_ms) end_ms = ev[i].ms;
    end_ms += 2000;
    for (int i = 0; i < nev; i++) if (!strcmp(ev[i].op, "end")) end_ms = ev[i].ms;
    FILE *w = fopen(out, "wb");
    if (!w) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    long frames = end_ms * MFE_SR / 1000 / MFE_BLOCK * MFE_BLOCK;
    fwrite("RIFF", 1, 4, w); put32(w, 36 + frames * 4); fwrite("WAVEfmt ", 1, 8, w);
    put32(w, 16); put16(w, 1); put16(w, 2); put32(w, MFE_SR); put32(w, MFE_SR * 4); put16(w, 4); put16(w, 16);
    fwrite("data", 1, 4, w); put32(w, frames * 4);
    int next = 0, peak = 0;
    int32_t buf[MFE_BLOCK * 2];
    for (long fr = 0; fr < frames; fr += MFE_BLOCK) {
        long now_ms = fr * 1000 / MFE_SR;
        while (next < nev && ev[next].ms <= now_ms) {
            ev_t *x = &ev[next++];
            if (!strcmp(x->op, "on")) mfe_note_on(e, (uint8_t)x->a, (uint8_t)x->b);
            else if (!strcmp(x->op, "off")) mfe_note_off(e, (uint8_t)x->a);
            else if (!strcmp(x->op, "cc")) { uint8_t m[3] = {0xB0, (uint8_t)x->a, (uint8_t)x->b}; mfe_midi(e, m, 3); }
            else if (!strcmp(x->op, "bend")) { int v = x->a + 8192; uint8_t m[3] = {0xE0, (uint8_t)(v & 127), (uint8_t)(v >> 7)}; mfe_midi(e, m, 3); }
        }
        mfe_render(e, buf);
        for (int i = 0; i < MFE_BLOCK * 2; i++) {
            put16(w, (uint16_t)(int16_t)buf[i]);
            int a = abs(buf[i]);
            if (a > peak) peak = a;
        }
    }
    fclose(w);
    printf("wrote %s: %.2f s, peak %.1f dBFS\n", out, (double)frames / MFE_SR, peak ? 20.0 * __builtin_log10(peak / 32768.0) : -999.0);
    return 0;
}

static int bench(mfe_engine_t *e, int voices)
{
    for (int i = 0; i < voices; i++) mfe_note_on(e, (uint8_t)(48 + i * 3), 100);
    int32_t buf[MFE_BLOCK * 2];
    long blocks = 10L * MFE_SR / MFE_BLOCK;     /* 10 s of audio */
    clock_t t0 = clock();
    for (long b = 0; b < blocks; b++) mfe_render(e, buf);
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%d voices (%u active): %.3f s CPU for 10 s audio = %.3f %% of real time on this PC "
           "(%.4f %% per voice incl. FX)\n", voices, mfe_active_voices(e), sec, sec * 10.0, sec * 10.0 / voices);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s PRESET.mfp|init SCORE|demo OUT.wav | --bench [voices]\n", argv[0]); return 2; }
    mfe_init_tables();
    mfe_patch_t p;
    if (load_preset(argv[1], &p)) return 1;
    mfe_engine_t *e = malloc(mfe_engine_size());
    mfe_engine_init(e);
    mfe_engine_set_patch(e, &p);
    if (!strcmp(argv[2], "--bench")) return bench(e, argc > 3 ? atoi(argv[3]) : 8);
    if (argc < 4) return 2;
    return render(e, argv[2], argv[3]);
}
