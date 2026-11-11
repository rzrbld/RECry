/*
 * recry-launcher - RECry's PortMaster launcher (C, SDL2, SDL2_ttf).
 *
 *   recry-launcher --root GAMEDIR
 *
 * First start (no gamedata/.installed): the unpack page. The user copies the
 * GOG installer (setup_far_cry_*.exe and its -1.bin) into GAMEDIR/installer/;
 * the launcher unpacks the game parts the port needs into GAMEDIR/gamedata/
 * with PortMaster's innoextract ($RECRY_INNO), with progress. A game copied
 * by hand into gamedata/ is accepted too.
 * Later starts: the settings page, then Start game / Exit.
 *
 * Settings are written to GAMEDIR/conf/recry.cfg (shell syntax, sourced by
 * the launch script). Everything the port writes stays inside GAMEDIR.
 * The launcher runs before gptokeyb starts and reads the controller itself.
 * Exit status: 0 start the game, 10 the user left, other values = error.
 *
 * Look: Far Cry's menus - an island at dusk, dark slate panels, amber
 * highlights - drawn here procedurally (no game art). The canvas is 480
 * pixels high and as wide as the screen's aspect ratio (at least 640), so
 * 4:3, 16:10 and 16:9 screens are filled without bars.
 *
 * Development: RECRY_WINDOWED=1 windowed; RECRY_LAUNCHER_SHOT=file.bmp saves
 * the third frame and exits; RECRY_LAUNCHER_PAGE=import|controls|done opens a
 * page; RECRY_SCREEN=WxH pretends the screen has that size; RECRY_AUTO_UNPACK=1
 * confirms the unpack pages (automated tests).
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define H 480
static int W = 640;                    /* logical width: 480 * screen aspect */
enum { EXIT_RUN = 0, EXIT_USER = 10, EXIT_ERR = 1 };

/* palette: Far Cry's menus */
static const SDL_Color C_TEXT = { 0xEE, 0xE8, 0xD8, 255 }, C_DIM = { 0x9A, 0xA6, 0xA0, 255 },
    C_AMBER = { 0xF0, 0xA4, 0x30, 255 }, C_AMBER_DK = { 0xB8, 0x6E, 0x14, 255 }, C_SEL_FG = { 0x14, 0x12, 0x0C, 255 },
    C_OK = { 0x8C, 0xD0, 0x62, 255 }, C_BAD = { 0xF0, 0x5A, 0x3C, 255 }, C_LOGO = { 0xF4, 0xEC, 0xD8, 255 };

static SDL_Renderer *R;
static SDL_Texture *g_bg;
static const char *g_root;
static char g_conf[4096], g_data[4096], g_inst[4096];
static int g_sw = 640, g_sh = 480;     /* real screen size */

/* ------------------------------------------------------------ text */
enum { F_BODY, F_SMALL, F_LOGO, F_TITLE, F_N };
static TTF_Font *g_font[F_N];
static void text_size(int f, const char *s, int *w, int *h) { *w = *h = 0; if (g_font[f]) TTF_SizeUTF8(g_font[f], s, w, h); }
static int text_w(int f, const char *s) { int w, h; text_size(f, s, &w, &h); return w; }
static void text(int f, int x, int y, const char *s, SDL_Color c) {
    if (!g_font[f] || !*s) return;
    SDL_Surface *sf = TTF_RenderUTF8_Blended(g_font[f], s, c);
    if (!sf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, sf);
    SDL_Rect d = { x, y, sf->w, sf->h };
    SDL_RenderCopy(R, t, NULL, &d);
    SDL_DestroyTexture(t); SDL_FreeSurface(sf);
}
static void text_c(int f, int y, const char *s, SDL_Color c) { text(f, (W - text_w(f, s)) / 2, y, s, c); }
static void text_r(int f, int xr, int y, const char *s, SDL_Color c) { text(f, xr - text_w(f, s), y, s, c); }
static void fill(int x, int y, int w, int h, SDL_Color c, int a) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, (Uint8)a); SDL_Rect r = { x, y, w, h }; SDL_RenderFillRect(R, &r);
}

/* ------------------------------------------------------- background */
static uint32_t g_rng = 0x5A17C0DEu;
static float frand(void) { g_rng = g_rng * 1664525u + 1013904223u; return (float)(g_rng >> 8) / 16777216.f; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static uint32_t rgb(float r, float g, float b) { return 0xFF000000u | (uint32_t)clampf(r, 0, 255) << 16 | (uint32_t)clampf(g, 0, 255) << 8 | (uint32_t)clampf(b, 0, 255); }
/* smooth 1D value noise for the island outline */
static float noise1(float x, const float *tab) { int i = (int)floorf(x); float f = x - i, s = f * f * (3 - 2 * f); return tab[i & 63] * (1 - s) + tab[(i + 1) & 63] * s; }
static void plot_disc(uint32_t *px, float cx, float cy, float r, uint32_t c) {
    for (int y = (int)(cy - r); y <= (int)(cy + r); y++) for (int x = (int)(cx - r); x <= (int)(cx + r); x++)
        if (x >= 0 && y >= 0 && x < W && y < H && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) px[y * W + x] = c;
}
/* a palm: a curved trunk and drooping fronds, as a silhouette */
static void palm(uint32_t *px, float bx, float by, float height, float lean, uint32_t c) {
    float tx = bx, ty = by;
    for (int i = 0; i <= 60; i++) {
        float t = i / 60.f;
        tx = bx + lean * height * t * t; ty = by - height * t;
        plot_disc(px, tx, ty, 3.2f - 1.4f * t, c);
    }
    for (int k = 0; k < 9; k++) {
        float a = -3.0f + k * 0.66f + (frand() - 0.5f) * 0.25f, len = height * (0.36f + frand() * 0.14f);
        float side = cosf(a) >= 0 ? 1.f : -1.f;
        for (int i = 0; i <= 50; i++) {
            float t = i / 50.f, x = tx + cosf(a) * len * t, y = ty + sinf(a) * len * t * 0.6f + len * 0.6f * t * t;
            plot_disc(px, x, y, 2.4f * (1 - t) + 0.8f, c);
            float leaf = (t < 0.1f ? 0 : sinf(t * 3.14159f)) * len * 0.16f;           /* leaflets hang below the rib */
            for (int j = 1; j <= (int)leaf; j++) {
                float lx = x + side * j * 0.35f, ly = y + j;
                if (lx >= 0 && ly >= 0 && lx < W && ly < H && ((int)(t * 50) % 2 == 0)) px[(int)ly * W + (int)lx] = c;
            }
        }
    }
}
static void make_background(void) {
    uint32_t *px = malloc((size_t)W * H * 4);
    const float horizon = H * 0.60f;
    /* sky: deep teal above, amber haze at the horizon; a low sun */
    float sunx = W * 0.70f, suny = horizon - 26;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float t = clampf(y / horizon, 0, 1), r, g, b;
        r = 18 + 230 * powf(t, 2.6f); g = 40 + 130 * powf(t, 2.6f); b = 58 + 30 * t;
        float d = hypotf(x - sunx, (y - suny) * 1.4f);
        float glow = expf(-d / 70.f) * 140 + expf(-d / 240.f) * 50;
        r += glow; g += glow * 0.72f; b += glow * 0.3f;
        if (d < 16) { r = 255; g = 226; b = 160; }
        if (y >= horizon) {                     /* sea: dark, with the sun's reflection */
            float s = (y - horizon) / (H - horizon);
            r = 10 + 40 * (1 - s); g = 30 + 46 * (1 - s); b = 38 + 40 * (1 - s);
            float refl = expf(-fabsf(x - sunx) / (18 + 60 * s)) * (frand() < 0.35f ? 1 : 0.25f) * (1 - s) * 190;
            r += refl; g += refl * 0.7f; b += refl * 0.35f;
        }
        float n = (frand() - 0.5f) * 7;
        px[y * W + x] = rgb(r + n, g + n, b + n);
    }
    /* islands: two layers of hills */
    float tab[64]; for (int i = 0; i < 64; i++) tab[i] = frand();
    for (int layer = 0; layer < 2; layer++) {
        uint32_t c = layer ? rgb(8, 16, 14) : rgb(28, 44, 44);
        float x0 = layer ? W * 0.04f : W * 0.30f, x1 = layer ? W * 0.46f : W * 0.98f, peak = layer ? 70 : 42;
        for (int x = (int)x0; x < (int)x1; x++) {
            float u = (x - x0) / (x1 - x0), env = sinf(u * 3.14159f);
            float hgt = env * (peak * (0.55f + 0.45f * noise1(u * 7 + layer * 13, tab)) + 6 * noise1(u * 40, tab));
            for (int y = (int)(horizon - hgt); y < (int)horizon + 2; y++) if (y >= 0 && y < H) px[y * W + x] = c;
        }
    }
    /* foreground beach and palms, black against the sky */
    uint32_t blk = rgb(4, 7, 6);
    for (int x = 0; x < W; x++) {
        float u = (float)x / W, top = H - 46 - 34 * (1 - u) * (1 - u) + 5 * noise1(u * 20, tab);
        for (int y = (int)top; y < H; y++) px[y * W + x] = blk;
    }
    palm(px, W * 0.07f, H - 70, 230, 0.32f, blk);
    palm(px, W * 0.16f, H - 60, 170, 0.18f, blk);
    palm(px, W * 0.93f, H - 50, 210, -0.30f, blk);
    /* vignette */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float dx = (x - W * 0.5f) / (W * 0.62f), dy = (y - H * 0.5f) / (H * 0.70f), v = 1 - 0.55f * clampf(dx * dx + dy * dy, 0, 1);
        uint32_t p = px[y * W + x];
        px[y * W + x] = rgb(((p >> 16) & 255) * v, ((p >> 8) & 255) * v, (p & 255) * v);
    }
    g_bg = SDL_CreateTexture(R, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
    SDL_UpdateTexture(g_bg, NULL, px, W * 4);
    free(px);
}

/* ------------------------------------------------------------ frame */
static int PX, PY, PW, PH;             /* the panel */
static void frame_begin(const char *title) {
    SDL_SetRenderDrawColor(R, 0, 0, 0, 255); SDL_RenderClear(R);
    if (g_bg) SDL_RenderCopy(R, g_bg, NULL, NULL);
    /* logo: weathered off-white capitals with a shadow, amber rule underneath */
    const char *logo = "RECry";
    int lw = text_w(F_LOGO, logo), lx = (W - lw) / 2;
    text(F_LOGO, lx + 3, 3, logo, (SDL_Color){ 0, 0, 0, 170 });
    text(F_LOGO, lx, 0, logo, C_LOGO);
    uint32_t s = 0x1234567u;                    /* scratches across the letters */
    for (int k = 0; k < 90; k++) {
        s = s * 1664525u + 1013904223u; int x = lx + (int)((s >> 8) % (unsigned)(lw ? lw : 1));
        s = s * 1664525u + 1013904223u; int y = 12 + (int)((s >> 8) % 50u);
        fill(x, y, 1 + (k % 3 == 0), 2 + (int)((s >> 4) % 5u), (SDL_Color){ 30, 26, 20, 255 }, 150);
    }
    fill(W / 2 - 120, 72, 240, 2, C_AMBER, 255);
    text_c(F_SMALL, 74, "Far Cry for PortMaster", C_AMBER);
    /* panel */
    PW = W - 96 > 600 ? 600 : W - 96; PX = (W - PW) / 2; PY = 98; PH = H - PY - 30;
    fill(PX, PY, PW, PH, (SDL_Color){ 8, 18, 20, 255 }, 205);
    fill(PX, PY, 3, PH, C_AMBER, 255);
    SDL_SetRenderDrawColor(R, 0x55, 0x66, 0x60, 120); SDL_Rect b = { PX, PY, PW, PH }; SDL_RenderDrawRect(R, &b);
    if (title) { text(F_TITLE, PX + 20, PY + 8, title, C_AMBER); fill(PX + 20, PY + 38, PW - 40, 1, C_AMBER_DK, 255); }
}
static void hint(const char *s) { text_c(F_SMALL, H - 24, s, C_DIM); }
static void present(void) {
    static int n; const char *shot = getenv("RECRY_LAUNCHER_SHOT");
    if (shot && ++n == 3) {
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
        SDL_SaveBMP(sf, shot); exit(0);
    }
    SDL_RenderPresent(R);
}

/* ------------------------------------------------------------ input */
enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_QUIT };
static SDL_GameController *g_pad;
static int g_axis_held;
static int poll_key(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: return K_QUIT;
        case SDL_CONTROLLERDEVICEADDED: if (!g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which); break;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN; case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT;
            case SDLK_RETURN: case SDLK_SPACE: return K_A; case SDLK_ESCAPE: case SDLK_BACKSPACE: return K_B;
            case SDLK_x: return K_X; case SDLK_y: return K_Y;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UP; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFT; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHT;
            case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START: return K_A;
            case SDL_CONTROLLER_BUTTON_B: return K_B;
            case SDL_CONTROLLER_BUTTON_X: return K_X; case SDL_CONTROLLER_BUTTON_Y: return K_Y;
            }
            break;
        case SDL_CONTROLLERAXISMOTION:          /* left stick navigates too */
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY || e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                int v = e.caxis.value, dir = 0;
                if (v < -20000) dir = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY ? K_UP : K_LEFT;
                else if (v > 20000) dir = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY ? K_DOWN : K_RIGHT;
                if (!dir) { g_axis_held = 0; break; }
                if (!g_axis_held) { g_axis_held = 1; return dir; }
            }
            break;
        }
    }
    return K_NONE;
}

/* ---------------------------------------------------------- settings */
static char g_res[32] = "auto";        /* auto or WxH */
static int g_stretch = 0, g_phys = 0, g_fx = 0, g_view = 0;   /* 0 auto, 1 on (simplified), 2 off (full) */
static int g_text = 0, g_limit = 30, g_fps = 0, g_cpu_perf = 1, g_controls = 0;   /* controls: 0 classic, 1 modern */

static void cfg_load(void) {
    char p[4200]; snprintf(p, sizeof p, "%s/recry.cfg", g_conf);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256], v[64];
    while (fgets(line, sizeof line, f)) {
        int n;
        if (sscanf(line, "RECRY_RES=%31s", v) == 1) snprintf(g_res, sizeof g_res, "%s", v);
        else if (sscanf(line, "RECRY_SCALE=%63s", v) == 1) g_stretch = !strcmp(v, "stretch");
        else if (sscanf(line, "RECRY_PHYSICS=%63s", v) == 1) g_phys = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 2 : 0;
        else if (sscanf(line, "RECRY_EFFECTS=%63s", v) == 1) g_fx = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 2 : 0;
        else if (sscanf(line, "RECRY_VIEW=%63s", v) == 1) g_view = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 2 : 0;
        else if (sscanf(line, "RECRY_TEXT=%63s", v) == 1) g_text = !strcmp(v, "auto") ? 0 : atoi(v);
        else if (sscanf(line, "RECRY_LIMIT=%d", &n) == 1) g_limit = n;
        else if (sscanf(line, "RECRY_FPS=%d", &n) == 1) g_fps = n < 0 || n > 2 ? 0 : n;   /* 0 off, 1 on, 2 built-in */
        else if (sscanf(line, "RECRY_CPU=%63s", v) == 1) g_cpu_perf = strcmp(v, "system") != 0;
        else if (sscanf(line, "RECRY_CONTROLS=%63s", v) == 1) g_controls = !strcmp(v, "modern");
    }
    fclose(f);
}
static const char *tri(int v) { return v == 1 ? "on" : v == 2 ? "off" : "auto"; }
static int cfg_save(void) {
    char p[4200], t[4300];
    snprintf(p, sizeof p, "%s/recry.cfg", g_conf); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w"); if (!f) return -1;
    char txt[16]; if (g_text) snprintf(txt, sizeof txt, "%d", g_text); else snprintf(txt, sizeof txt, "auto");
    fprintf(f, "# RECry settings (written by the launcher)\n"
               "RECRY_RES=%s\nRECRY_SCALE=%s\nRECRY_PHYSICS=%s\nRECRY_EFFECTS=%s\nRECRY_VIEW=%s\n"
               "RECRY_TEXT=%s\nRECRY_LIMIT=%d\nRECRY_FPS=%d\nRECRY_CPU=%s\nRECRY_CONTROLS=%s\n",
            g_res, g_stretch ? "stretch" : "fit", tri(g_phys), tri(g_fx), tri(g_view), txt, g_limit, g_fps, g_cpu_perf ? "perf" : "system",
            g_controls ? "modern" : "classic");
    if (fclose(f) || rename(t, p)) return -1;
    return 0;
}

/* render resolutions offered: the screen's own aspect at common heights, plus 4:3 sizes */
static char g_resl[24][32]; static int g_resn;
static void auto_res(char *out, size_t n) {
    int h = g_sh <= 480 ? g_sh : 480, w = (int)((double)h * g_sw / g_sh + 1) & ~1;
    snprintf(out, n, "%dx%d", w, h);
}
static void build_res_list(void) {
    static const int hs[] = { 360, 400, 480, 540, 600, 720, 768, 800, 900, 1080 };
    g_resn = 0; snprintf(g_resl[g_resn++], 32, "auto");
    int native_43 = g_sw * 3 == g_sh * 4;
    for (size_t i = 0; i < sizeof hs / sizeof *hs; i++) {
        if (hs[i] > g_sh) break;
        int w = (int)((double)hs[i] * g_sw / g_sh + 1) & ~1;
        snprintf(g_resl[g_resn++], 32, "%dx%d", w, hs[i]);
    }
    if (g_sh != hs[0] && g_sh != 400 && g_sh != 480 && g_sh != 540 && g_sh != 600 && g_sh != 720 && g_sh != 768 && g_sh != 800 && g_sh != 900 && g_sh != 1080 && g_resn < 22)
        snprintf(g_resl[g_resn++], 32, "%dx%d", g_sw, g_sh);
    if (!native_43) {                         /* 4:3 for a different picture shape */
        static const int f43[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 } };
        for (int i = 0; i < 3 && g_resn < 24; i++) if (f43[i][1] <= g_sh) snprintf(g_resl[g_resn++], 32, "%dx%d", f43[i][0], f43[i][1]);
    }
}
static int res_index(void) { for (int i = 0; i < g_resn; i++) if (!strcmp(g_resl[i], g_res)) return i; return 0; }

static const char *k_layout_name[2] = { "Classic", "Modern shooter" };
static int page_controls(void) {
    static const char *classic[][2] = {
        { "Left stick", "Move" }, { "Right stick", "Look" }, { "RT", "Fire" }, { "LT", "Aim / zoom" },
        { "A", "Jump" }, { "B", "Use, enter vehicles" }, { "X", "Reload" }, { "Y", "Next weapon" },
        { "LB / RB", "Lean left / right" }, { "L3", "Sprint" }, { "R3", "Crouch / prone" },
        { "D-pad left", "Previous weapon" }, { "D-pad up", "Fire mode" }, { "D-pad down", "Throw grenade" },
        { "Select", "Objectives / score" }, { "Start", "Pause menu" } };
    static const char *modern[][2] = {
        { "Left stick", "Move" }, { "Right stick", "Look" }, { "RT / LT", "Fire / aim" }, { "A", "Jump" },
        { "B", "Crouch (hold)" }, { "X", "Reload" }, { "Y", "Next weapon" }, { "RB", "Throw grenade" },
        { "L3 / R3", "Sprint / flashlight" }, { "D-pad up", "Binoculars" }, { "D-pad down", "Grenade type" },
        { "D-pad L / R", "Lean left / right" }, { "Select", "Pause menu" }, { "Start", "Objectives / score" },
        { "LB + B / X", "Prone / use" }, { "LB + Y", "Fire mode" }, { "LB + R stick", "Lean" } };
    int which = g_controls;
    for (;;) {
        char title[64]; snprintf(title, sizeof title, "CONTROLS: %s", k_layout_name[which]);
        for (char *c = title; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        frame_begin(title);
        const char *(*rows)[2] = which ? modern : classic;
        const int n = which ? (int)(sizeof modern / sizeof *modern) : (int)(sizeof classic / sizeof *classic);
        int half = (n + 1) / 2, colw = (PW - 40) / 2;
        for (int i = 0; i < n; i++) {
            int c = i / half, r = i % half, x = PX + 20 + c * colw, y = PY + 48 + r * 28;
            text(F_SMALL, x, y, rows[i][0], C_AMBER); text(F_SMALL, x + 110, y, rows[i][1], C_TEXT);
        }
        text_c(F_SMALL, PY + PH - 26, which == g_controls ? "This preset is selected (Settings > Controls)" : "Select it in Settings > Controls", C_DIM);
        hint("Left/Right other preset    A / B back");
        present();
        int k = poll_key();
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_LEFT || k == K_RIGHT) which = !which;
        if (k == K_A || k == K_B) return -1;
        SDL_Delay(16);
    }
}

static int page_settings(void) {
    enum { S_RES, S_SCALE, S_PHYS, S_FX, S_VIEW, S_TEXT, S_LIMIT, S_FPS, S_CPU, S_CTRL, S_RUN, S_EXIT, S_N };
    static const char *lab[S_N] = { "Resolution", "Picture", "Physics", "Effects", "View distance", "Menu text size",
                                    "Frame limit", "FPS counter", "CPU", "Controls", "START GAME", "Exit" };
    int sel = S_RUN, ri = res_index();
    for (;;) {
        frame_begin("SETTINGS");
        char v[S_N][48]; memset(v, 0, sizeof v);  /* rows without a value: START GAME, Exit */
        if (!strcmp(g_resl[ri], "auto")) { char a[32]; auto_res(a, sizeof a); snprintf(v[S_RES], 48, "Auto (%s)", a); }
        else snprintf(v[S_RES], 48, "%s", g_resl[ri]);
        snprintf(v[S_SCALE], 48, "%s", g_stretch ? "Stretch to screen" : "Keep aspect");
        static const char *tri3[3][3] = { { "Auto", "Simplified", "Full" }, { "Auto", "Reduced", "Full" }, { "Auto", "Reduced", "Full" } };
        snprintf(v[S_PHYS], 48, "%s", tri3[0][g_phys]); snprintf(v[S_FX], 48, "%s", tri3[1][g_fx]); snprintf(v[S_VIEW], 48, "%s", tri3[2][g_view]);
        if (g_text) snprintf(v[S_TEXT], 48, "%d %%", g_text); else snprintf(v[S_TEXT], 48, "Auto");
        if (g_limit) snprintf(v[S_LIMIT], 48, "%d FPS", g_limit); else snprintf(v[S_LIMIT], 48, "Off");
        snprintf(v[S_FPS], 48, "%s", g_fps == 1 ? "On" : g_fps == 2 ? "Built-in" : "Off");
        snprintf(v[S_CPU], 48, "%s", g_cpu_perf ? "Performance" : "System");
        snprintf(v[S_CTRL], 48, "%s", k_layout_name[g_controls]);
        int rowh = 22, y0 = PY + 46;
        for (int i = 0; i < S_N; i++) {
            int y = y0 + i * rowh + (i >= S_CTRL ? 4 : 0) + (i >= S_RUN ? 4 : 0);
            int on = i == sel;
            if (on) fill(PX + 12, y - 1, PW - 24, rowh, C_AMBER, 235);
            SDL_Color fg = on ? C_SEL_FG : (i == S_RUN ? C_AMBER : C_TEXT);
            text(F_BODY, PX + 24, y, lab[i], fg);
            if (v[i][0]) {
                int xr = PX + PW - 28;
                text_r(F_BODY, xr, y, ">", on ? C_SEL_FG : C_AMBER);
                int vw = text_w(F_BODY, v[i]), xv = xr - 18 - vw;
                text(F_BODY, xv, y, v[i], fg);
                text(F_BODY, xv - 18, y, "<", on ? C_SEL_FG : C_AMBER);
            }
        }
        const char *h =
            sel == S_RES ? "Game resolution. Lower is faster; the picture is scaled to the screen" :
            sel == S_SCALE ? (g_stretch ? "Fill the whole screen (the picture may look stretched)" : "Keep the game's shape, black bars if the screen differs") :
            sel == S_PHYS ? "Simplified: idle objects sleep, fewer physics steps. Auto = simplified on slow handhelds" :
            sel == S_FX ? "Reduced: fewer particles and decals, no water reflections. Auto: by device" :
            sel == S_VIEW ? "Reduced: shorter draw distance for props (not enemies). Auto: by device" :
            sel == S_TEXT ? "Size of menu text in the game. Auto = bigger on small screens" :
            sel == S_LIMIT ? (g_limit == 0 ? "Off: uncapped, as fast as the device can" : g_limit == 60 ? "60 FPS cap: for fast devices" : "30 FPS cap: steady pacing on handhelds") :
            sel == S_FPS ? (g_fps == 2 ? "Built-in: the engine's own statistics (FPS, draw calls, memory)" : "On: a small frame rate counter in the top right corner") :
            sel == S_CPU ? "Performance: CPU at full rated speed while playing (restored on exit)" :
            sel == S_CTRL ? (g_controls ? "Modern shooter: B crouch, RB grenade, LB layer. A shows the layout" : "Classic: the port's original layout. A shows the layout") :
            sel == S_RUN ? "Save these settings and start Far Cry" : "Leave without starting the game";
        fill(PX + 20, PY + PH - 30, PW - 40, 1, C_AMBER_DK, 255);
        text_c(F_SMALL, PY + PH - 24, h, C_DIM);
        hint("Up/Down select    Left/Right change    A confirm    B exit");
        present();
        int k = poll_key();
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_UP) sel = (sel + S_N - 1) % S_N;
        if (k == K_DOWN) sel = (sel + 1) % S_N;
        if (k == K_LEFT || k == K_RIGHT) {
            int d = k == K_RIGHT ? 1 : -1;
            switch (sel) {
            case S_RES: ri = (ri + g_resn + d) % g_resn; snprintf(g_res, sizeof g_res, "%s", g_resl[ri]); break;
            case S_SCALE: g_stretch = !g_stretch; break;
            case S_PHYS: g_phys = (g_phys + 3 + d) % 3; break;
            case S_FX: g_fx = (g_fx + 3 + d) % 3; break;
            case S_VIEW: g_view = (g_view + 3 + d) % 3; break;
            case S_TEXT: { static const int T[] = { 0, 100, 115, 130, 150 }; int i = 0; while (i < 5 && T[i] != g_text) i++; if (i == 5) i = 0; g_text = T[(i + 5 + d) % 5]; break; }
            case S_LIMIT: { static const int L[] = { 30, 60, 0 }; int i = g_limit == 60 ? 1 : g_limit == 0 ? 2 : 0; g_limit = L[(i + 3 + d) % 3]; break; }
            case S_FPS: g_fps = (g_fps + 3 + d) % 3; break;
            case S_CPU: g_cpu_perf = !g_cpu_perf; break;
            case S_CTRL: g_controls = !g_controls; break;
            }
        }
        if (k == K_A) {
            if (sel == S_CTRL) { int r = page_controls(); if (r >= 0) return r; }
            else if (sel == S_RUN) return cfg_save() == 0 ? EXIT_RUN : EXIT_ERR;
            else if (sel == S_EXIT) return EXIT_USER;
        }
        if (k == K_B) return EXIT_USER;
        SDL_Delay(16);
    }
}

/* ----------------------------------------------------------- unpack */
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }
typedef struct { char exe[4200]; int bins; long long bytes; } setup_info;
static setup_info find_setup(void) {
    setup_info si; memset(&si, 0, sizeof si);
    DIR *d = opendir(g_inst); if (!d) return si;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncasecmp(e->d_name, "setup_far_cry", 13)) continue;
        char p[4200]; snprintf(p, sizeof p, "%s/%s", g_inst, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (ends_with(e->d_name, ".exe")) snprintf(si.exe, sizeof si.exe, "%s", p);
        else if (ends_with(e->d_name, ".bin")) si.bins++;
        si.bytes += st.st_size;
    }
    closedir(d);
    return si;
}
static long long free_mb(const char *path) { struct statvfs vs; return statvfs(path, &vs) ? -1 : (long long)vs.f_bavail * vs.f_frsize >> 20; }
/* the parts of the installer the port uses (paths inside the installer) */
static const char *k_parts[] = { "FCData", "Levels", "Shaders", "Languages", "DevMode.lua", "__support/app/game.cfg" };
#define NPARTS (int)(sizeof k_parts / sizeof *k_parts)
static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int ok = 0;
    while (!ok && (e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; }
    closedir(d); return ok;
}
/* a usable game in gamedata/: the four folders, the archives and game.cfg */
static int game_complete(char *missing, size_t n) {
    static const char *dirs[] = { "FCData", "Levels", "Shaders", "Languages" };
    char p[4200], q[4300];
    for (int i = 0; i < 4; i++) if (!find_ci(g_data, dirs[i], p, sizeof p)) { snprintf(missing, n, "%s/", dirs[i]); return 0; }
    find_ci(g_data, "FCData", p, sizeof p);
    static const char *paks[] = { "Scripts.pak", "Objects.pak", "Textures.pak", "Sounds.pak" };
    for (int i = 0; i < 4; i++) if (!find_ci(p, paks[i], q, sizeof q)) { snprintf(missing, n, "FCData/%s", paks[i]); return 0; }
    if (!find_ci(g_data, "game.cfg", p, sizeof p)) { snprintf(missing, n, "game.cfg"); return 0; }
    return 1;
}
static void mark_installed(void) { char m[4200]; snprintf(m, sizeof m, "%s/.installed", g_data); FILE *f = fopen(m, "w"); if (f) fclose(f); }

#define LOGN 6
static char g_log[LOGN][96];
static void log_push(const char *s) { memmove(g_log[0], g_log[1], sizeof g_log[0] * (LOGN - 1)); snprintf(g_log[LOGN - 1], sizeof g_log[0], "%s", s); }
/* run innoextract with the part filters; lines of " - "..." are files */
static int run_inno(const char *setup, int list_only, int total, int *count) {
    const char *inno = getenv("RECRY_INNO");
    if (!inno || access(inno, X_OK)) { log_push("PortMaster's innoextract was not found (update PortMaster)"); return -1; }
    const char *av[32]; int ac = 0;
    av[ac++] = inno; av[ac++] = list_only ? "-l" : "-e"; av[ac++] = "-c"; av[ac++] = "0"; av[ac++] = "-p"; av[ac++] = "0";
    if (!list_only) { av[ac++] = "-d"; av[ac++] = g_data; }
    for (int i = 0; i < NPARTS; i++) { av[ac++] = "-I"; av[ac++] = k_parts[i]; }
    av[ac++] = setup; av[ac] = NULL;
    int pfd[2]; if (pipe(pfd)) return -1;
    pid_t pid = fork();
    if (pid == 0) { dup2(pfd[1], 1); dup2(pfd[1], 2); close(pfd[0]); execv(inno, (char *const *)av); _exit(127); }
    close(pfd[1]); fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    char buf[2048]; size_t bl = 0; int status = -1, exited = 0; *count = 0;
    Uint32 t0 = SDL_GetTicks();
    for (;;) {
        ssize_t r = read(pfd[0], buf + bl, sizeof buf - 1 - bl);
        if (r > 0) {
            bl += (size_t)r; buf[bl] = 0; char *nl;
            while ((nl = strchr(buf, '\n'))) {
                *nl = 0;
                if (!list_only) printf("%s\n", buf);
                if (!strncmp(buf, " - \"", 4)) {
                    (*count)++;
                    if (!list_only) { char *q = strchr(buf + 4, '"'); if (q) *q = 0; log_push(buf + 4); }
                } else if (buf[0] && !list_only) log_push(buf);
                memmove(buf, nl + 1, strlen(nl + 1) + 1); bl = strlen(buf);
            }
            if (bl == sizeof buf - 1) bl = 0;
        }
        if (!exited) { int st; if (waitpid(pid, &st, WNOHANG) == pid) { status = WIFEXITED(st) ? WEXITSTATUS(st) : -1; exited = 1; } }
        if (exited && r <= 0) break;
        if (!list_only) {
            Uint32 s = (SDL_GetTicks() - t0) / 1000;
            frame_begin("UNPACKING THE GAME");
            char l[96]; snprintf(l, sizeof l, "%d of %d files      %02u:%02u", *count, total, s / 60, s % 60);
            text(F_BODY, PX + 24, PY + 52, l, C_TEXT);
            int bw = PW - 48; float f = total ? (float)*count / total : 0; if (f > 1) f = 1;
            fill(PX + 24, PY + 84, bw, 14, (SDL_Color){ 30, 40, 40, 255 }, 255);
            fill(PX + 24, PY + 84, (int)(bw * f), 14, C_AMBER, 255);
            for (int i = 0; i < LOGN; i++) text(F_SMALL, PX + 24, PY + 112 + i * 20, g_log[i], i == LOGN - 1 ? C_TEXT : C_DIM);
            hint("This takes a while. Keep the handheld powered");
            present();
            SDL_Event e; while (SDL_PollEvent(&e)) {}
        } else { SDL_Event e; while (SDL_PollEvent(&e)) {} }
        if (r <= 0) SDL_Delay(40);
    }
    close(pfd[0]);
    return status;
}
static int unpack(const char *setup) {
    memset(g_log, 0, sizeof g_log);
    frame_begin("UNPACKING THE GAME"); text(F_BODY, PX + 24, PY + 52, "Reading the installer...", C_TEXT); present();
    int total = 0, n = 0;
    if (run_inno(setup, 1, 0, &total) != 0 || !total) { log_push("innoextract could not read the installer"); return -1; }
    mkdir(g_data, 0755);
    if (run_inno(setup, 0, total, &n) != 0) return -1;
    /* game.cfg: from the installer's support folder to the game folder */
    char a[4200], b[4200];
    snprintf(a, sizeof a, "%s/__support/app/game.cfg", g_data); snprintf(b, sizeof b, "%s/game.cfg", g_data);
    if (rename(a, b) && access(b, F_OK)) { log_push("game.cfg missing in the installer"); return -1; }
    snprintf(a, sizeof a, "%s/__support/app", g_data); rmdir(a);
    snprintf(a, sizeof a, "%s/__support", g_data); rmdir(a);
    /* the "FCData" filter also matches the installer's app/FCData placeholder folders */
    snprintf(a, sizeof a, "%s/app", g_data);
    if (!access(a, F_OK)) { pid_t pid = fork(); if (pid == 0) { execl("/bin/rm", "rm", "-rf", a, (char *)NULL); _exit(127); } if (pid > 0) waitpid(pid, NULL, 0); }
    char miss[64];
    if (!game_complete(miss, sizeof miss)) { char m[128]; snprintf(m, sizeof m, "incomplete after unpacking: %s", miss); log_push(m); return -1; }
    mark_installed();
    return 0;
}
static int page_done(const setup_info *si) {
    for (;;) {
        frame_begin("GAME UNPACKED");
        text(F_BODY, PX + 24, PY + 54, "Far Cry is ready to play.", C_OK);
        char l[160]; snprintf(l, sizeof l, "The installer in ports/recry/installer/ is no longer needed (%.1f GB).", si->bytes / 1073741824.0);
        text(F_SMALL, PX + 24, PY + 92, l, C_TEXT);
        text(F_SMALL, PX + 24, PY + 114, "Keep it as a backup, or delete it to free the space.", C_DIM);
        fill(PX + 12, PY + 158, PW - 24, 26, C_AMBER, 235);
        text(F_BODY, PX + 24, PY + 160, "A    Keep the installer and continue", C_SEL_FG);
        text(F_BODY, PX + 24, PY + 196, "Y    Delete the installer and continue", C_TEXT);
        hint("A keep    Y delete");
        present();
        int k = poll_key();
        if (getenv("RECRY_AUTO_UNPACK")) k = K_A;                             /* automated tests: keep */
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_A || k == K_B) return -1;
        if (k == K_Y) {
            DIR *d = opendir(g_inst); struct dirent *e;
            if (d) { while ((e = readdir(d))) if (!strncasecmp(e->d_name, "setup_far_cry", 13)) { char p[4200]; snprintf(p, sizeof p, "%s/%s", g_inst, e->d_name); unlink(p); } closedir(d); }
            return -1;
        }
        SDL_Delay(16);
    }
}
static int page_import(void) {
    for (;;) {
        setup_info si = find_setup();
        char miss[64]; int have_game = game_complete(miss, sizeof miss);
        long long fm = free_mb(g_root);
        frame_begin("FIRST START: UNPACK YOUR GAME");
        text(F_SMALL, PX + 24, PY + 50, "Copy your GOG Far Cry installer (both files) into ports/recry/installer/:", C_TEXT);
        text(F_SMALL, PX + 40, PY + 76, si.exe[0] ? "[x]  setup_far_cry_*.exe" : "[  ]  setup_far_cry_*.exe", si.exe[0] ? C_OK : C_BAD);
        text(F_SMALL, PX + 40, PY + 98, si.bins ? "[x]  setup_far_cry_*-1.bin" : "[  ]  setup_far_cry_*-1.bin", si.bins ? C_OK : C_BAD);
        char l[128]; snprintf(l, sizeof l, "Free space: %.1f GB (4 GB needed)", fm / 1024.0);
        text(F_SMALL, PX + 40, PY + 120, l, fm >= 4096 ? C_OK : C_BAD);
        text(F_SMALL, PX + 24, PY + 150, "Or copy an installed Far Cry into ports/recry/gamedata/ (FCData, Levels,", C_DIM);
        text(F_SMALL, PX + 24, PY + 168, "Shaders, Languages and game.cfg).", C_DIM);
        int ready = si.exe[0] && si.bins && fm >= 4096;
        const char *act = have_game ? "A    Use the game in gamedata/" : ready ? "A    Unpack the game" : "A    Look again";
        fill(PX + 12, PY + 206, PW - 24, 26, C_AMBER, 235);
        text(F_BODY, PX + 24, PY + 208, act, C_SEL_FG);
        text(F_BODY, PX + 24, PY + 244, "B    Exit", C_TEXT);
        hint("A continue    B exit");
        present();
        int k = poll_key();
        if (getenv("RECRY_AUTO_UNPACK") && (ready || have_game)) k = K_A;   /* automated tests */
        if (k == K_QUIT || k == K_B) return EXIT_USER;
        if (k == K_A && have_game) { mark_installed(); return -1; }
        if (k == K_A && ready) {
            if (unpack(si.exe) == 0) return page_done(&si);
            for (;;) {
                frame_begin("UNPACKING FAILED");
                for (int i = 0; i < LOGN; i++) text(F_SMALL, PX + 24, PY + 54 + i * 20, g_log[i], i == LOGN - 1 ? C_BAD : C_DIM);
                text(F_SMALL, PX + 24, PY + 190, "Full log: ports/recry/log.txt", C_DIM);
                fill(PX + 12, PY + 218, PW - 24, 26, C_AMBER, 235);
                text(F_BODY, PX + 24, PY + 220, "A    Try again", C_SEL_FG);
                text(F_BODY, PX + 24, PY + 256, "B    Exit", C_TEXT);
                hint("A retry    B exit");
                present();
                int k2 = poll_key();
                if (getenv("RECRY_AUTO_UNPACK")) return EXIT_ERR;               /* automated tests: report */
                if (k2 == K_A) break;
                if (k2 == K_B || k2 == K_QUIT) return EXIT_USER;
                SDL_Delay(16);
            }
        }
        SDL_Delay(16);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--root")) g_root = argv[i + 1];
    if (!g_root) { fprintf(stderr, "usage: recry-launcher --root GAMEDIR\n"); return EXIT_ERR; }
    snprintf(g_conf, sizeof g_conf, "%s/conf", g_root);
    snprintf(g_data, sizeof g_data, "%s/gamedata", g_root);
    snprintf(g_inst, sizeof g_inst, "%s/installer", g_root);
    mkdir(g_conf, 0755); mkdir(g_inst, 0755);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return EXIT_ERR; }
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) { g_sw = dm.w; g_sh = dm.h; }
    if (getenv("RECRY_SCREEN")) sscanf(getenv("RECRY_SCREEN"), "%dx%d", &g_sw, &g_sh);
    if (g_sw < g_sh) { int t = g_sw; g_sw = g_sh; g_sh = t; }          /* portrait panels are rotated by the firmware */
    W = (int)((double)H * g_sw / g_sh + 0.5); if (W < 640) W = 640; W &= ~1;
    uint32_t fl = getenv("RECRY_WINDOWED") ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_Window *win = SDL_CreateWindow("RECry", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, fl);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return EXIT_ERR; }
    R = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!R) R = SDL_CreateRenderer(win, -1, 0);
    if (!R) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return EXIT_ERR; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(R, W, H);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    if (TTF_Init() == 0) {
        static const char *files[F_N] = { "NotoSans-Bold.ttf", "NotoSans-Regular.ttf", "NotoSansDisplay-Bold.ttf", "NotoSansDisplay-Bold.ttf" };
        static const int pt[F_N] = { 15, 13, 58, 19 };
        for (int i = 0; i < F_N; i++) {
            char fp[4200]; snprintf(fp, sizeof fp, "%s/fonts/%s", g_root, files[i]);
            g_font[i] = TTF_OpenFont(fp, pt[i]);
            if (!g_font[i]) fprintf(stderr, "[launcher] %s: %s\n", fp, TTF_GetError());
        }
    }
    make_background();
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++) if (SDL_IsGameController(i)) g_pad = SDL_GameControllerOpen(i);
    cfg_load();
    build_res_list();
    if (strcmp(g_res, "auto") && res_index() == 0) snprintf(g_res, sizeof g_res, "auto");   /* saved size not offered on this screen */
    int rc = -1;
    char marker[4200]; snprintf(marker, sizeof marker, "%s/.installed", g_data);
    const char *pg = getenv("RECRY_LAUNCHER_PAGE");
    if (pg && !strcmp(pg, "controls")) rc = page_controls();
    else if (pg && !strcmp(pg, "import")) rc = page_import();
    else if (pg && !strcmp(pg, "done")) { setup_info si = find_setup(); rc = page_done(&si); }
    else if (access(marker, F_OK) != 0) rc = page_import();
    else if (getenv("RECRY_AUTOSTART")) rc = cfg_save() == 0 ? EXIT_RUN : EXIT_ERR;
    if (rc < 0) rc = page_settings();
    SDL_DestroyRenderer(R); SDL_DestroyWindow(win); SDL_Quit();
    return rc;
}
