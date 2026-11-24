/* FastStore - store PS4 pour GoldHEN. Domaine public. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Net.h>
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Pad.h>
#include <orbis/ImeDialog.h>
#include <orbis/AppInstUtil.h>
#include "cJSON.h"

#define POOL (4*1024*1024)
#define BUF  (1024*1024)
#define SEGS 16
#define CHUNK (8*1024*1024)
#define CW 260
#define CH 360
#define GAP 28
#define COLS 5
#define TOPY 190
#define DATA "/data/faststore/"

typedef struct { char id[16], name[160], region[8], cover[512], url[3][512];
                 int nurl; long long size; } game_t;

typedef struct { game_t *g; int segs; volatile int stop, state;
                 char out[300], err[96]; int64_t done, size; double speed;
                 pthread_t th[SEGS]; int64_t st[SEGS], en[SEGS]; } job_t;

static game_t *G; static int NG, NV; static int *IDX;
static SDL_Renderer *R; static TTF_Font *F[4];
static const int FSZ[4] = { 18, 22, 28, 42 };
static job_t *JOB; static int INSTALLED;
static char QUERY[64]; static int FOCUS, MODE; static float SCROLL, TSCROLL;
static int PADH = -1;

/* ---------- texte ---------- */
typedef struct { char s[80]; int sz; SDL_Texture *t; int w, h; } tc_t;
static tc_t TC[96];
static SDL_Color lastc;

static void txt(const char *s, int sz, SDL_Color c, int x, int y, int maxw){
    for (int i = 0; i < 96; i++)
        if (TC[i].t && TC[i].sz == sz && !strcmp(TC[i].s, s)
            && TC[i].t && lastc.r == c.r && lastc.g == c.g){
            SDL_Rect d = { x, y, maxw > 0 && TC[i].w > maxw ? maxw : TC[i].w, TC[i].h };
            SDL_RenderCopy(R, TC[i].t, NULL, &d); return;
        }
    SDL_Surface *sf = TTF_RenderUTF8_Blended(F[sz], s, c);
    if (!sf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, sf);
    int slot = 0; for (int i = 0; i < 96; i++) if (!TC[i].t){ slot = i; break; }
    if (TC[slot].t) SDL_DestroyTexture(TC[slot].t);
    snprintf(TC[slot].s, 80, "%s", s); TC[slot].sz = sz; TC[slot].t = t;
    TC[slot].w = sf->w; TC[slot].h = sf->h;
    SDL_Rect d = { x, y, maxw > 0 && sf->w > maxw ? maxw : sf->w, sf->h };
    SDL_RenderCopy(R, t, NULL, &d);
    SDL_FreeSurface(sf);
}

/* ---------- formes ---------- */
static void rrect(int x, int y, int w, int h, int r, SDL_Color c){
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_Rect a = { x + r, y, w - 2*r, h }; SDL_RenderFillRect(R, &a);
    SDL_Rect b = { x, y + r, w, h - 2*r }; SDL_RenderFillRect(R, &b);
    for (int i = 0; i < r; i++){
        int d = r - (int)sqrtf((float)(r*r - i*i));
        SDL_Rect u = { x + d, y + r - 1 - i, w - 2*d, 1 };
        SDL_Rect v = { x + d, y + h - r + i, w - 2*d, 1 };
        SDL_RenderFillRect(R, &u); SDL_RenderFillRect(R, &v);
    }
}

/* ---------- reseau ---------- */
static int net_init(void){
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_NET);
    if (sceNetInit() < 0) return -1;
    sceNetPoolCreate("pool", POOL, 0);
    sceSslInit(POOL); sceHttpInit(POOL);
    return 0;
}

static int fetch(const char *url, char **out, size_t *len){
    int tpl = 0, conn = 0, req = 0, st = 0, n;
    size_t cap = 1 << 20, got = 0;
    char *p = malloc(cap); if (!p) return -1;
    if ((tpl = sceHttpCreateTemplate("FastStore/1.0", 1, 1)) < 0) goto fail;
    sceHttpSetRecvTimeOut(tpl, 30);
    if ((conn = sceHttpCreateConnectionWithURL(tpl, url, 0)) < 0) goto fail;
    if ((req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url, 0)) < 0) goto fail;
    if (sceHttpSendRequest(req, NULL, 0) < 0) goto fail;
    sceHttpGetStatusCode(req, &st);
    if (st != 200) goto fail;
    while ((n = sceHttpReadData(req, p + got, 65536)) > 0){
        got += n;
        if (got + 65536 > cap){ cap *= 2; p = realloc(p, cap); }
    }
    sceHttpDeleteRequest(req); sceHttpDeleteConnection(conn); sceHttpDeleteTemplate(tpl);
    p[got] = 0; *out = p; *len = got; return 0;
fail:
    if (req) sceHttpDeleteRequest(req);
    if (conn) sceHttpDeleteConnection(conn);
    if (tpl) sceHttpDeleteTemplate(tpl);
    free(p); return -1;
}

static int range_get(const char *url, int64_t from, int64_t to, int fd,
                     int64_t *pos, volatile int *stop){
    int tpl = 0, conn = 0, req = 0, st = 0, n, ret = -1;
    char h[64], *buf = malloc(BUF);
    if (!buf) return -1;
    snprintf(h, sizeof h, "bytes=%lld-%lld", (long long)from, (long long)to);
    if ((tpl = sceHttpCreateTemplate("FastStore/1.0", 1, 1)) < 0) goto out;
    sceHttpSetRecvTimeOut(tpl, 30);
    if ((conn = sceHttpCreateConnectionWithURL(tpl, url, 0)) < 0) goto out;
    if ((req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url, 0)) < 0) goto out;
    sceHttpAddRequestHeader(req, "Range", h, 0);
    if (sceHttpSendRequest(req, NULL, 0) < 0) goto out;
    sceHttpGetStatusCode(req, &st);
    if (st != 206 && st != 200) goto out;
    while ((n = sceHttpReadData(req, buf, BUF)) > 0){
        if (*stop) break;
        lseek(fd, *pos, SEEK_SET);
        int w = 0;
        while (w < n){ int r = write(fd, buf + w, n - w); if (r <= 0) goto out; w += r; }
        *pos += n;
        if (*pos > to) break;
    }
    ret = 0;
out:
    if (req) sceHttpDeleteRequest(req);
    if (conn) sceHttpDeleteConnection(conn);
    if (tpl) sceHttpDeleteTemplate(tpl);
    free(buf); return ret;
}

/* ---------- telechargement ---------- */
static void *seg_worker(void *arg){
    int i = (int)(intptr_t)arg; job_t *j = JOB;
    int64_t pos = j->st[i], before; int fails = 0;
    int fd = open(j->out, O_WRONLY);
    if (fd < 0){ j->state = 3; strcpy(j->err, "ecriture impossible"); return NULL; }
    while (pos <= j->en[i] && !j->stop){
        int64_t end = j->en[i];
        if (end - pos + 1 > CHUNK) end = pos + CHUNK - 1;
        before = pos;
        if (range_get(j->g->url[0], pos, end, fd, &pos, &j->stop) == 0){
            __sync_fetch_and_add(&j->done, pos - before); fails = 0;
        } else {
            __sync_fetch_and_add(&j->done, pos - before);
            if (++fails > 5){ j->state = 3; strcpy(j->err, "erreur reseau"); break; }
            sceKernelUsleep(400000 * fails);
        }
    }
    close(fd); return NULL;
}

static void job_start(game_t *g){
    job_t *j = calloc(1, sizeof *j);
    j->g = g; j->size = g->size;
    snprintf(j->out, sizeof j->out, "/mnt/usb0/%s.pkg", g->id);
    int fd = open(j->out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0){ ftruncate(fd, g->size); close(fd); }
    j->segs = SEGS;
    int64_t per = g->size / SEGS;
    for (int i = 0; i < SEGS; i++){
        j->st[i] = i * per;
        j->en[i] = (i == SEGS - 1) ? g->size - 1 : (i + 1) * per - 1;
    }
    j->state = 1; INSTALLED = 0;
    JOB = j;
    for (int i = 0; i < SEGS; i++)
        pthread_create(&j->th[i], NULL, seg_worker, (void *)(intptr_t)i);
}

static void job_speed(void){
    if (!JOB || JOB->state != 1) return;
    static int64_t last; static int64_t lastt; static int once;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t now = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    if (!once){ lastt = now; once = 1; return; }
    if (now - lastt >= 500){
        JOB->speed = (JOB->done - last) / ((now - lastt) / 1000.0) / 1048576.0;
        last = JOB->done; lastt = now;
    }
    if (JOB->done >= JOB->size && JOB->state == 1) JOB->state = 2;
}

/* ---------- catalogue ---------- */
static int icontains(const char *h, const char *n){
    size_t l = strlen(n); if (!l) return 1;
    for (; *h; h++){
        size_t i = 0;
        while (i < l && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) i++;
        if (i == l) return 1;
    }
    return 0;
}

static void filter(const char *q){
    NV = 0;
    for (int i = 0; i < NG; i++)
        if (!q[0] || icontains(G[i].name, q) || icontains(G[i].id, q)) IDX[NV++] = i;
    FOCUS = 0; SCROLL = TSCROLL = 0;
}

static int catalog_load(void){
    char *raw = NULL; size_t len = 0;
    FILE *f = fopen(DATA "catalog.json", "rb");
    if (f){
        fseek(f, 0, SEEK_END); len = ftell(f); rewind(f);
        raw = malloc(len + 1); fread(raw, 1, len, f); raw[len] = 0; fclose(f);
    } else {
        char url[512] = {0}; FILE *u = fopen(DATA "url.txt", "r");
        if (u){ fgets(url, sizeof url, u); fclose(u);
            url[strcspn(url, "\r\n")] = 0;
            if (fetch(url, &raw, &len) < 0) return -1;
        } else return -1;
    }
    cJSON *root = cJSON_Parse(raw); free(raw);
    if (!root) return -1;
    cJSON *arr = cJSON_GetObjectItem(root, "games");
    NG = cJSON_GetArraySize(arr);
    G = calloc(NG, sizeof(game_t)); IDX = calloc(NG, sizeof(int));
    for (int i = 0; i < NG; i++){
        cJSON *e = cJSON_GetArrayItem(arr, i), *v; game_t *g = &G[i];
        if ((v = cJSON_GetObjectItem(e, "id")))     snprintf(g->id, 16, "%s", v->valuestring);
        if ((v = cJSON_GetObjectItem(e, "name")))   snprintf(g->name, 160, "%s", v->valuestring);
        if ((v = cJSON_GetObjectItem(e, "region"))) snprintf(g->region, 8, "%s", v->valuestring);
        if ((v = cJSON_GetObjectItem(e, "cover")))  snprintf(g->cover, 512, "%s", v->valuestring);
        if ((v = cJSON_GetObjectItem(e, "size")))   g->size = (long long)v->valuedouble;
        cJSON *u = cJSON_GetObjectItem(e, "urls");
        for (int k = 0; k < cJSON_GetArraySize(u) && k < 3; k++)
            snprintf(g->url[g->nurl++], 512, "%s", cJSON_GetArrayItem(u, k)->valuestring);
    }
    cJSON_Delete(root);
    filter("");
    return 0;
}

/* jaquettes */
static struct { char id[16]; SDL_Texture *t; } COV[96];
static SDL_Texture *cover(const game_t *g){
    for (int i = 0; i < 96; i++) if (COV[i].t && !strcmp(COV[i].id, g->id)) return COV[i].t;
    char p[300]; snprintf(p, sizeof p, DATA "cache/%s.jpg", g->id);
    if (access(p, R_OK) != 0) return NULL;
    SDL_Texture *t = IMG_LoadTexture(R, p);
    if (!t) return NULL;
    int slot = 0; for (int i = 0; i < 96; i++) if (!COV[i].t){ slot = i; break; }
    if (COV[slot].t) SDL_DestroyTexture(COV[slot].t);
    snprintf(COV[slot].id, 16, "%s", g->id); COV[slot].t = t;
    return t;
}

/* ---------- rendu ---------- */
static void draw_card(int i, int x, int y){
    game_t *g = &G[IDX[i]]; int foc = (i == FOCUS);
    float k = foc ? 1.06f : 1.0f;
    int w = (int)(CW * k), h = (int)(CH * k);
    int cx = x - (w - CW) / 2, cy = y - (h - CH) / 2;
    rrect(cx + 6, cy + 10, w, h, 22, (SDL_Color){0,0,0,110});
    rrect(cx, cy, w, h, 22, (SDL_Color){26,29,38,255});
    SDL_Texture *c = cover(g);
    if (c){
        SDL_Rect d = { cx + 6, cy + 6, w - 12, h - 80 };
        SDL_RenderCopy(R, c, NULL, &d);
    } else {
        rrect(cx + 6, cy + 6, w - 12, h - 80, 16, (SDL_Color){38,44,58,255});
        txt("PS4", 3, (SDL_Color){66,76,96,255}, cx + 24, cy + 110, 0);
    }
    txt(g->name, 1, (SDL_Color){235,238,245,255}, cx + 14, cy + h - 66, w - 28);
    char m[48]; snprintf(m, sizeof m, "%s  %.1f Go", g->region, g->size / 1073741824.0);
    txt(m, 0, (SDL_Color){128,138,156,255}, cx + 14, cy + h - 38, 0);
    if (foc){
        SDL_SetRenderDrawColor(R, 40, 190, 255, 255);
        for (int k2 = 0; k2 < 3; k2++){
            SDL_Rect o = { cx - k2, cy - k2, w + 2*k2, h + 2*k2 };
            SDL_RenderDrawRect(R, &o);
        }
    }
}

static void overlay(void){
    SDL_SetRenderDrawColor(R, 0, 0, 0, 205);
    SDL_Rect f = { 0, 0, 1920, 1080 }; SDL_RenderFillRect(R, &f);
    rrect(560, 390, 800, 300, 26, (SDL_Color){24,27,35,255});
    txt(JOB && JOB->state == 2 ? "Telechargement termine" : "Telechargement",
        2, (SDL_Color){240,244,250,255}, 600, 428, 0);
    txt(JOB ? JOB->g->name : "", 1, (SDL_Color){168,178,196,255}, 600, 474, 720);
    rrect(600, 530, 720, 18, 9, (SDL_Color){40,45,58,255});
    if (JOB && JOB->size > 0){
        int fw = (int)(720.0 * JOB->done / JOB->size);
        if (fw > 0) rrect(600, 530, fw, 18, 9, (SDL_Color){40,190,255,255});
    }
    char s[160];
    snprintf(s, sizeof s, "%.0f %%   %.2f Mo/s   %lld / %lld Mo",
             JOB && JOB->size ? 100.0 * JOB->done / JOB->size : 0.0,
             JOB ? JOB->speed : 0.0,
             (long long)(JOB ? JOB->done / 1048576 : 0),
             (long long)(JOB ? JOB->size / 1048576 : 0));
    txt(s, 1, (SDL_Color){200,210,225,255}, 600, 566, 0);
    if (JOB && JOB->state == 2) txt("Installation en cours…", 1, (SDL_Color){90,220,140,255}, 600, 610, 0);
    if (JOB && JOB->state == 3) txt(JOB->err, 1, (SDL_Color){235,110,110,255}, 600, 610, 0);
}

static void frame(void){
    for (int y = 0; y < 1080; y += 4){
        SDL_SetRenderDrawColor(R, 14, 16, 22, 255);
        SDL_Rect l = { 0, y, 1920, 4 }; SDL_RenderFillRect(R, &l);
    }
    rrect(0, 0, 1920, 5, 0, (SDL_Color){32,180,255,40});
    txt("FASTSTORE", 3, (SDL_Color){240,244,250,255}, 70, 48, 0);
    char n[48]; snprintf(n, sizeof n, "%d jeux", NV);
    txt(n, 1, (SDL_Color){118,130,148,255}, 70, 102, 0);
    rrect(560, 50, 800, 64, 16, (SDL_Color){24,27,35,255});
    txt(QUERY[0] ? QUERY : "Rechercher un jeu…   (TRIANGLE)",
        1, (SDL_Color){QUERY[0] ? 235 : 96, QUERY[0] ? 240 : 106,
                       QUERY[0] ? 250 : 124, 255}, 584, 70, 760);

    SCROLL += (TSCROLL - SCROLL) * 0.22f;
    for (int i = 0; i < NV; i++){
        int x = 70 + (i % COLS) * (CW + GAP);
        int y = TOPY + (i / COLS) * (CH + GAP) - (int)SCROLL;
        if (y > 1100 || y + CH < 160) continue;
        draw_card(i, x, y);
    }
    SDL_SetRenderDrawColor(R, 10, 12, 17, 255);
    SDL_Rect b = { 0, 1010, 1920, 70 }; SDL_RenderFillRect(R, &b);
    txt("X  Installer     CARRE  Details     TRIANGLE  Rechercher     ROND  Annuler",
        0, (SDL_Color){150,160,178,255}, 70, 1032, 0);
    if (MODE == 2) overlay();
}

/* ---------- entree ---------- */
static unsigned pad_read(float *ly){
    OrbisPadData d; *ly = 0;
    if (PADH < 0) return 0;
    if (scePadReadState(PADH, &d) != 0) return 0;
    *ly = (d.leftStick.y - 128) / 128.0f;
    return d.buttons;
}

static void do_install(void){
    if (INSTALLED || !JOB) return;
    sceAppInstUtilInitialize();
    sceAppInstUtilAppInstallPkg(JOB->out, 0);
    INSTALLED = 1;
}

int main(void){
    mkdir(DATA, 0777); mkdir(DATA "cache", 0777);
    net_init();
    scePadInit();
    for (int i = 0; i < 4 && PADH < 0; i++)
        PADH = scePadOpen(0x1000000 + i * 0x1000000, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *w = SDL_CreateWindow("FastStore", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN);
    R = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    TTF_Init();
    for (int i = 0; i < 4; i++) F[i] = TTF_OpenFont("/app0/assets/font.ttf", FSZ[i]);

    if (catalog_load() < 0){ NG = NV = 0; G = NULL; IDX = NULL; }

    unsigned prev = 0;
    for (;;){
        float ly; unsigned btn = pad_read(&ly);
        unsigned hit = btn & ~prev; prev = btn;
        static unsigned held; held = btn;

        if (MODE != 2){
            if (hit & ORBIS_PAD_BUTTON_RIGHT && FOCUS % COLS < COLS - 1 && FOCUS + 1 < NV) FOCUS++;
            if (hit & ORBIS_PAD_BUTTON_LEFT && FOCUS % COLS > 0) FOCUS--;
            if (hit & ORBIS_PAD_BUTTON_DOWN && FOCUS + COLS < NV) FOCUS += COLS;
            if (hit & ORBIS_PAD_BUTTON_UP && FOCUS >= COLS) FOCUS -= COLS;
            if (ly < -0.4f) TSCROLL -= 22;
            if (ly > 0.4f) TSCROLL += 22;
            TSCROLL = (FOCUS / COLS) * (CH + GAP) - 200;
            if (TSCROLL < 0) TSCROLL = 0;

            if (hit & ORBIS_PAD_BUTTON_TRIANGLE){
                OrbisImeDialogParam p; memset(&p, 0, sizeof p);
                p.userId = 0x1000000; p.type = ORBIS_IME_TYPE_BASIC_LATIN;
                p.maxLength = 63; p.inputTextBuffer = QUERY;
                p.title = "Rechercher"; p.posx = 560; p.posy = 200;
                p.horizontalAlignment = ORBIS_IME_HALIGN_CENTER;
                QUERY[0] = 0; sceImeDialogInit(&p, NULL);
            }
            if (sceImeDialogGetStatus() == ORBIS_IME_DIALOG_STATUS_FINISHED){
                OrbisImeDialogResult res; memset(&res, 0, sizeof res);
                sceImeDialogGetResult(&res);
                if (res.endstatus == ORBIS_IME_DIALOG_END_STATUS_OK)
                    snprintf(QUERY, 64, "%s", res.imeTextBuffer);
                sceImeDialogTerm(); filter(QUERY);
            }
            if ((hit & ORBIS_PAD_BUTTON_CROSS) && NV > 0 && !JOB) { job_start(&G[IDX[FOCUS]]); MODE = 2; }
        } else {
            job_speed();
            if (JOB && JOB->state == 2 && !INSTALLED) do_install();
            if (hit & ORBIS_PAD_BUTTON_CIRCLE){ if (JOB) JOB->stop = 1; JOB = NULL; MODE = 0; }
        }

        SDL_RenderClear(R);
        frame();
        SDL_RenderPresent(R);
        SDL_Delay(16);
    }
}