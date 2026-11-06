// Colheita de PGS do anel do leitor (subtap): janela sintetica, sem rede.
// A fonte imita o fx_buf do dts_engine: bytes de um arquivo inteiro, uma
// janela [lo, hi) que avanca como o leitor do video (passos de 64 KiB) e salta
// no seek com troca de geracao; `ler` copia sem consumir e nunca espera. Os
// cantos que a revisao cobrou:
//   1. fluxo inteiro com janelas atravessando Cluster e bloco; Cluster de
//      tamanho desconhecido fechando no proximo nivel 1; bytes arrastados da
//      janela bem menores que o arquivo (o video de 200 KiB por Cluster nao e
//      lido);
//   2. seek no MEIO do payload do bloco de video: ressincronizar acha o
//      cabecalho do proximo Cluster e entrega so o que vem depois dele;
//   3. troca de faixa no meio do fluxo: cobertura zera e so a nova entrega;
//   4. bloco laceado da faixa alvo: descartado e a cobertura do Cluster dele
//      NAO vale (prova com plano pequeno e cobertura exata);
//   5. fonte encerrada com o fio vivo (video fechou, fio da legenda existia);
//   6. parar enquanto o fio dorme esperando dado.
#include "subtap.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TQ(cond, ...) do { if (!(cond)) { printf("FALHOU (%d): ", __LINE__); printf(__VA_ARGS__); putchar('\n'); exit(1); } } while (0)
#define JANELA (64L * 1024)

// --- construtor de Matroska --------------------------------------------------

typedef struct { unsigned char *p; size_t n, cap; } Buf;

static void ba(Buf *b, const void *d, size_t k) {
  if (b->n + k > b->cap) { b->cap = (b->n + k) * 2 + 1024; b->p = realloc(b->p, b->cap); assert(b->p); }
  if (k) memcpy(b->p + b->n, d, k);
  b->n += k;
}
static void bStart(Buf *b) { b->p = NULL; b->n = b->cap = 0; }
static void bFree(Buf *b) { free(b->p); bStart(b); }
static void zera(Buf *b, size_t k) {
  static const unsigned char z[4096];
  while (k) { size_t c = k < sizeof z ? k : sizeof z; ba(b, z, c); k -= c; }
}
// Tamanho: vint de 4 bytes com o marcador (o mesmo estilo do mkvass_pgs).
static void vsize(Buf *b, size_t v) {
  unsigned char x[4] = { (unsigned char)(0x10 | ((v >> 24) & 0x0F)),
                         (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
  ba(b, x, 4);
}
// ID EBML em complemento (o numero magico ja traz o bit marcador).
static void vid(Buf *b, unsigned long id) {
  unsigned char v[4]; int n = 0, i;
  for (i = 0; id; i++) { v[3 - i] = (unsigned char)(id & 0xFF); id >>= 8; }
  n = i > 4 ? 4 : i;
  for (i = 0; i < n; i++) ba(b, v + 4 - n + i, 1);
}
static void elx(Buf *out, unsigned long id, const unsigned char *d, size_t n) {
  vid(out, id); vsize(out, n); ba(out, d, n);
}
static void elB(Buf *out, unsigned long id, const Buf *body) { elx(out, id, body->p, body->n); }
static void elU(Buf *b, unsigned long id, unsigned long v) {
  unsigned char x[8]; int i, n = v > 0xFFFFFF ? 4 : v > 0xFFFF ? 3 : v > 0xFF ? 2 : 1;
  for (i = 0; i < n; i++) x[i] = (unsigned char)(v >> (8 * (n - 1 - i)));
  elx(b, id, x, (size_t)n);
}
// Tamanho desconhecido (vint de 8 bytes, todos os bits de valor em 1).
static void bTamX(Buf *b) { unsigned char x[8] = {1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; ba(b, x, 8); }

// --- payload PGS: o mesmo display set de tests/mkvass_pgs --------------------

static const unsigned char RLE[17] = {0x01,0x00,0x83,0x02,0x00,0x02,0x00,0x00,0x00,0x40,0x03,0x00,0xC0,0x03,0x01,0x00,0x00};

static void seg(Buf *b, unsigned char tipo, const unsigned char *d, size_t n) {
  unsigned char h[3] = { tipo, (unsigned char)(n >> 8), (unsigned char)(n & 0xFF) };
  ba(b, h, 3); if (n) ba(b, d, n);
}
// PCS de 1920x1080; com objetos: um, id 7, em (100, 900).
static void pcseg(Buf *b, unsigned char estado, int n) {
  unsigned char d[19];
  d[0]=1920>>8; d[1]=1920&0xFF; d[2]=1080>>8; d[3]=1080&0xFF;
  d[4]=0x10; d[5]=0; d[6]=1; d[7]=estado; d[8]=0; d[9]=0; d[10]=(unsigned char)n;
  if (n) {
    d[11]=0; d[12]=7; d[13]=0; d[14]=0;
    d[15]=100>>8; d[16]=100&0xFF; d[17]=900>>8; d[18]=900&0xFF;
  }
  seg(b, 0x16, d, n ? 19 : 11);
}
static void pgsMostra(Buf *b) {
  static const unsigned char pds[12] = {0,0,1,235,128,128,255,2,16,128,128,128};
  unsigned char od[28];
  pcseg(b, 0x80, 1);
  seg(b, 0x14, pds, 12);
  od[0]=0; od[1]=7; od[2]=0; od[3]=0xC0;                    // id 7, versao, 1o+ult frag
  od[4]=0; od[5]=0; od[6]=(unsigned char)(4 + sizeof RLE);  // total do RLE (24 bits)
  od[7]=0; od[8]=6; od[9]=0; od[10]=2;                      // 6x2
  memcpy(od + 11, RLE, sizeof RLE);
  seg(b, 0x15, od, 28);
  seg(b, 0x80, NULL, 0);                                    // END
}
static void pgsLimpa(Buf *b) { pcseg(b, 0x80, 0); seg(b, 0x80, NULL, 0); }

// Corpo de SimpleBlock/Block: vint(faixa) + rel int16 BE + flags + payload.
static void blocoCorpo(Buf *b, int faixa, long rel, const unsigned char *pay, size_t n, int laced) {
  unsigned char h[4] = { (unsigned char)(0x80 | faixa), (unsigned char)(rel >> 8),
                         (unsigned char)rel, (unsigned char)(laced ? 0x06 : 0) };
  ba(b, h, 4); if (n) ba(b, pay, n);
}

// --- o arquivo sintetico -----------------------------------------------------
//
// Faixa 1 = video (bloco de `vkb` por Cluster: a faixa ERRADA, grande de pro-
// pósito, para provar que os bytes dela nao saem da janela). Faixa 6 = PGS.
// Faixa 2 = segunda PGS (troca de faixa). `dur >= 0` vira BlockGroup (0xA0,
// Block 0xA1 + BlockDuration 0x9B); `dur < 0` vira SimpleBlock (fim = -1).

#define FAIXA_VIDEO 1
#define FAIXA_PGS   6
#define FAIXA_PGS2  2

typedef struct { int faixa; long rel; long dur; int limpa; int laced; } Sub;

typedef struct {
  Buf f;
  int64_t cl[6];          // inicio de cada Cluster
  int64_t videoFim[6];    // fim do bloco de video de cada Cluster
  int64_t videoMid[6];    // ponto DENTRO do payload (para o seek)
} Fixt;

// Anexa as legendas [a, b) de uma faixa de descritores no corpo do Cluster.
static void subsCria(Buf *c, const Sub *s, int a, int b) {
  for (int j = a; j < b && s[j].faixa; j++) {
    Buf pay, bl; bStart(&pay); bStart(&bl);
    if (s[j].limpa) pgsLimpa(&pay); else pgsMostra(&pay);
    blocoCorpo(&bl, s[j].faixa, s[j].rel, pay.p, pay.n, s[j].laced);
    if (s[j].dur >= 0) {
      Buf bg; bStart(&bg);
      elB(&bg, 0xA1, &bl);
      elU(&bg, 0x9B, (unsigned long)s[j].dur);       // BlockDuration
      elB(c, 0xA0, &bg); bFree(&bg);                 // BlockGroup
    } else {
      elB(c, 0xA3, &bl);                             // SimpleBlock
    }
    bFree(&bl); bFree(&pay);
  }
}

// Anexa UM bloco de video da faixa ERRADA (`vkb` de payload zorado).
static void videoBito(Buf *c, size_t vkb) {
  vid(c, 0xA3); vsize(c, 4 + vkb);
  blocoCorpo(c, FAIXA_VIDEO, 0, NULL, 0, 0);
  zera(c, vkb);
}

static void trackEntry(Buf *c, int numero, int tipo, const char *codec) {
  Buf t; bStart(&t);
  elU(&t, 0xD7, (unsigned long)numero);              // TrackNumber
  elU(&t, 0x83, (unsigned long)tipo);                // TrackType
  elx(&t, 0x86, (const unsigned char *)codec, strlen(codec));
  elB(c, 0xAE, &t); bFree(&t);                       // TrackEntry
}

// montaF: Segment(Tracks + Clusters) com offsets absolutos anotados.
// `desco` = Cluster de tamanho DESCONHECIDO (-1 nenhum); `subsPrimeiro[i]`
// coloca a primeira legenda ANTES do video (o Cluster pos-seek: o bloco da
// faixa alvo tem de caber na MESMA janela de 64 KiB que o cabecalho que o
// resync procura). Offsets 0..: cl/videoFim/videoMid por Cluster.
static void montaF(Fixt *F, int nCl, const unsigned long *ts, const Sub (*subs)[2],
                   const int *subsPrimeiro, int desco, size_t vkb) {
  Buf tr, clB[6];
  int64_t vFimRel[6], vMidRel[6], contTam;
  memset(F, 0, sizeof *F);
  bStart(&tr);
  { Buf te; bStart(&te);
    trackEntry(&te, FAIXA_VIDEO, 1, "V_MPEG4/ISO/AVC");
    trackEntry(&te, FAIXA_PGS, 17, "S_HDMV/PGS");
    trackEntry(&te, FAIXA_PGS2, 17, "S_HDMV/PGS");
    elB(&tr, 0x1654AE6BUL, &te); bFree(&te);
  }
  contTam = (int64_t)tr.n;
  for (int i = 0; i < nCl; i++) {
    Buf *c = &clB[i]; bStart(c);
    { Buf t2; bStart(&t2); elU(&t2, 0xE7, ts[i]); ba(c, t2.p, t2.n); bFree(&t2); }
    vFimRel[i] = vMidRel[i] = -1;
    if (subsPrimeiro && subsPrimeiro[i]) {
      subsCria(c, subs[i], 0, 1);              // legenda antes do video
      videoBito(c, vkb);
      subsCria(c, subs[i], 1, 2);              // a segunda legenda depois
    } else {
      int64_t base = (int64_t)c->n;
      videoBito(c, vkb);
      TQ((int64_t)c->n == base + 5 + 4 + (int64_t)vkb, "tamanho do video %d", i);
      vFimRel[i] = (int64_t)c->n;              // fim do ELEMENTO de video
      vMidRel[i] = vFimRel[i] - 16384;         // 16 KiB antes do fim do payload
      if (vMidRel[i] < base) vMidRel[i] = base;
      subsCria(c, subs[i], 0, 2);
    }
    contTam += 4 + (i == desco ? 8 : 4) + (int64_t)c->n;
  }
  // Montagem final: cabeca do Segmento, Tracks e os Clusters com offsets.
  bStart(&F->f);
  vid(&F->f, 0x18538067UL);                      // Segment
  vsize(&F->f, (size_t)contTam);
  ba(&F->f, tr.p, tr.n); bFree(&tr);
  for (int i = 0; i < nCl; i++) {
    int horn = 4 + (i == desco ? 8 : 4);
    F->cl[i] = (int64_t)F->f.n;
    vid(&F->f, 0x1F43B675UL);
    if (i == desco) bTamX(&F->f); else vsize(&F->f, (size_t)clB[i].n);
    ba(&F->f, clB[i].p, clB[i].n);
    if (vFimRel[i] >= 0) {
      F->videoFim[i] = F->cl[i] + horn + vFimRel[i];
      F->videoMid[i] = F->cl[i] + horn + vMidRel[i];
    } else {
      F->videoFim[i] = F->videoMid[i] = -1;
    }
    bFree(&clB[i]);
  }
}

// --- fonte sintetica (o "anel") ----------------------------------------------

typedef struct {
  pthread_mutex_t trava;
  const unsigned char *buf;
  int64_t n;                    // o arquivo inteiro
  int64_t lo, hi;               // a janela [lo, hi) que o leitor tem na memoria
  uint64_t gen;                 // sobe no seek (descontinuidade)
} Fonte;

static int fJanela(void *ctx, int64_t *lo, int64_t *hi, uint64_t *gen) {
  Fonte *F = ctx;
  pthread_mutex_lock(&F->trava);
  *lo = F->lo; *hi = F->hi; *gen = F->gen;
  pthread_mutex_unlock(&F->trava);
  return 1;
}
static int fLer(void *ctx, int64_t off, unsigned char *dst, int k) {
  Fonte *F = ctx; int d = 0;
  pthread_mutex_lock(&F->trava);
  if (off >= F->lo && off < F->hi) {
    int64_t pode = F->hi - off;
    if (pode < k) k = (int)pode;
    memcpy(dst, F->buf + off, (size_t)k);
    d = k;
  }
  pthread_mutex_unlock(&F->trava);
  return d;                     // 0 = byte ainda fora da memoria do leitor
}
static void fEncerrar(void *ctx) { (void)ctx; }
static void fLiberar(void *ctx) { (void)ctx; }

static const SubtapFonteOps fOps = { fJanela, fLer, fEncerrar, fLiberar };

static Fonte *fonteNova(const unsigned char *buf, int64_t n) {
  Fonte *F = calloc(1, sizeof *F);
  TQ(F != NULL, "fonte: sem memoria");
  pthread_mutex_init(&F->trava, NULL);
  F->buf = buf; F->n = n; F->lo = F->hi = 0; F->gen = 1;
  return F;
}
// Janela NOVA (o seek do player: a geracao sobe, e descontinuidade).
static void janelaSet(Fonte *F, int64_t lo, int64_t hi) {
  pthread_mutex_lock(&F->trava);
  F->lo = lo; F->hi = hi; F->gen++;
  pthread_mutex_unlock(&F->trava);
}
static SubtapFonte *fonteNovaSub(Fonte **out, const unsigned char *buf, int64_t n) {
  Fonte *F = fonteNova(buf, n);
  *out = F;
  return subtap_fonte_nova(&fOps, F);
}
static void janelaAumenta(Fonte *F, int64_t hi) {
  pthread_mutex_lock(&F->trava);
  if (hi > F->hi) F->hi = hi;
  pthread_mutex_unlock(&F->trava);
}

// --- captura das entregas ----------------------------------------------------

typedef struct {
  double ini, fim;              // o cue
  int n, tw, th, x, y, w, h;    // a composicao e o primeiro objeto
  unsigned char r0, r3, r7;     // pixels de prova do rgba
} Mold;

static pthread_mutex_t capturaTrava = PTHREAD_MUTEX_INITIALIZER;
static Mold mold[96];
static int moldN;

static void seguir(void *ctx, double ini, double fim, PgsQuadro *q) {
  (void)ctx;
  pthread_mutex_lock(&capturaTrava);
  if (moldN < 96) {
    Mold *m = &mold[moldN++];
    m->ini = ini; m->fim = fim;
    m->n = q->n; m->tw = q->tela_w; m->th = q->tela_h;
    if (q->n) {
      m->x = q->obj[0].x; m->y = q->obj[0].y;
      m->w = q->obj[0].w; m->h = q->obj[0].h;
      m->r0 = q->obj[0].rgba[0]; m->r3 = q->obj[0].rgba[3]; m->r7 = q->obj[0].rgba[7];
    }
  }
  pthread_mutex_unlock(&capturaTrava);
  pgs_quadro_liberar(q);        // a posse passa pelo chamador
}

static void capturaZera(void) {
  pthread_mutex_lock(&capturaTrava);
  moldN = 0;
  pthread_mutex_unlock(&capturaTrava);
}
static int capturaN(void) {
  int n;
  pthread_mutex_lock(&capturaTrava);
  n = moldN;
  pthread_mutex_unlock(&capturaTrava);
  return n;
}

// --- esperas e conferencias --------------------------------------------------

static long agoraMs(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
// 1 = chegou; 0 = estourou o limite.
static int esperarEntregas(int n, long limiteMs, long *gasto) {
  long t0 = agoraMs(), decorrido;
  while (capturaN() < n) {
    decorrido = agoraMs() - t0;
    if (decorrido > limiteMs) { if (gasto) *gasto = decorrido; return 0; }
    usleep(10000);
  }
  if (gasto) *gasto = agoraMs() - t0;
  return 1;
}
static int mesmoTempo(double a, double b) { return fabs(a - b) <= 1e-6; }

static void conferirMostra(const Mold *m) {
  TQ(m->n == 1 && m->tw == 1920 && m->th == 1080, "mostra: n=%d tela %dx%d", m->n, m->tw, m->th);
  TQ(m->x == 100 && m->y == 900 && m->w == 6 && m->h == 2, "mostra: em %d,%d %dx%d", m->x, m->y, m->w, m->h);
  TQ(m->r0 == 255 && m->r3 == 255 && m->r7 == 128, "mostra: rgba %d %d %d", m->r0, m->r3, m->r7);
}
// Conferir a entrega `i` (mostra ou limpa) com os valores esperados.
static void conferir(int i, double ini, double fim, int mostra) {
  Mold m;
  TQ(capturaN() > i, "entrega %d ausente (teve %d)", i, capturaN());
  pthread_mutex_lock(&capturaTrava);
  m = mold[i];
  pthread_mutex_unlock(&capturaTrava);
  TQ(mesmoTempo(m.ini, ini) && mesmoTempo(m.fim, fim), "entrega %d: ini %.3f fim %.3f (esperado %.3f e %.3f)",
     i, m.ini, m.fim, ini, fim);
  if (mostra) conferirMostra(&m);
  else TQ(m.n == 0, "limpa %d: n=%d (deveria compor vazio)", i, m.n);
}
// Espera a cobertura ficar (1, de, ate]. Sobrou limite a mais: o fecho do
// Cluster anda um ciclo de fio depois do display set chegar.
static int esperarCobertura(Subtap *t, double de, double ate, long limiteMs) {
  long t0 = agoraMs();
  double d = -1, a = -1;
  for (;;) {
    if (subtap_cobertura(t, &d, &a) && mesmoTempo(d, de) && mesmoTempo(a, ate)) return 1;
    if (agoraMs() - t0 > limiteMs) {
      double x = 0, y = 0;
      int tem = subtap_cobertura(t, &x, &y);
      printf("[cobertura] esperado (%.3f, %.3f) teve %s (%.3f, %.3f)\n",
             de, ate, tem ? "sim" : "nao", x, y);
      return 0;
    }
    usleep(10000);
  }
}

// --- planos dos arquivos -----------------------------------------------------

// Arquivo grande (6 Clusters, video de 200 KiB cada):
//   C0 ts1000: mostra 1.25..3.0 (BlockGroup + duracao 1750)
//   C1 ts5000: limpa 5.5 (SimpleBlock)
//   C2 ts7000: mostra 9.7..10.4 — CLUSTER DE TAMANHO DESCONHECIDO
//   C3 ts12000: laceado DESCARTADO + mostra 12.8..13.3
//   C4 ts18000: limpa da FAIXA 2 em 18.2 (faixa errada para quem colhe a 6)
//   C5 ts24000: mostra 24.3 (faixa 6) e limpa 24.55 (faixa 2) — dados do
//               seek e da troca; a legenda vem ANTES do video (os dois TEM
//               de caber na mesma janela que o cabecalho do resync)
static const Sub subsA[6][2] = {
  { { FAIXA_PGS,  250, 1750, 0, 0 }, { 0 } },
  { { FAIXA_PGS,  500,   -1, 1, 0 }, { 0 } },
  { { FAIXA_PGS, 2700,  700, 0, 0 }, { 0 } },
  { { FAIXA_PGS,    0,   -1, 0, 1 }, { FAIXA_PGS, 800, 500, 0, 0 } },
  { { FAIXA_PGS2, 200,   -1, 1, 0 }, { 0 } },
  { { FAIXA_PGS,  300,   -1, 0, 0 }, { FAIXA_PGS2, 550, -1, 1, 0 } },
};
static const unsigned long tsA[6] = { 1000, 5000, 7000, 12000, 18000, 24000 };
static const int    subsPri[6]   = { 0, 0, 0, 0, 0, 1 };

// Arquivo pequeno (3 Clusters de 4 KiB de video): a prova da cobertura.
//   A ts1000: mostra 1.25..3.0   B ts5000: SÓ o laceado   C ts9000: limpa 9.5
static const Sub subsB[3][2] = {
  { { FAIXA_PGS, 250, 1750, 0, 0 }, { 0 } },
  { { FAIXA_PGS,   0,   -1, 0, 1 }, { 0 } },
  { { FAIXA_PGS, 500,   -1, 1, 0 }, { 0 } },
};
static const unsigned long tsB[3] = { 1000, 5000, 9000 };

// --- o arranque de cada cenario ----------------------------------------------

static Subtap *comeco(SubtapFonte *nt, Fonte *F, int faixa) {
  janelaSet(F, 0, 0);           // recomeca: fio novo nao deve somar buraco
  capturaZera();
  Subtap *t = subtap_iniciar(nt, faixa, 1000000UL, seguir, NULL);
  TQ(t != NULL, "fio nao subiu");
  return t;
}

// Revela a janela em passos de 64 KiB (como o leitor do video anda) ate `n`
// entregas. `primeiro` = comece do zero; senao, continue a janela atual. Se a
// Mais um passo aparece a cada 60 ms. So no fim da janela a espera usa o
// prazo de falha; a falta de legenda em video intercalado nao atrasa o teste.
static void colhePasso(Fonte *F, int64_t limite, int n, int primeiro) {
  long gasto;
  if (primeiro) janelaSet(F, 0, JANELA < limite ? JANELA : limite);
  while (capturaN() < n) {
    if (!esperarEntregas(n, 60, &gasto)) {
      int64_t cur, hi;
      pthread_mutex_lock(&F->trava);
      cur = F->hi;
      pthread_mutex_unlock(&F->trava);
      if (cur >= limite) {
        TQ(esperarEntregas(n, 2000, &gasto), "entregas paradas em %d de %d (janela toda revelada, %ld ms)",
           capturaN(), n, gasto);
        break;
      }
      hi = cur + JANELA; if (hi > limite) hi = limite;
      janelaAumenta(F, hi);
    }
  }
}

// --- cenarios ----------------------------------------------------------------

// 1: fluxo inteiro; bytes da janela bem menores que o arquivo. O C2 tinha
// tamanho desconhecido e fechou no cabecalho do C3; o laceado do C3 descarta
// so o bloco dele, a entrega dele segue; o C5 (legenda ANTES do video) sai.
static void cenarioFluxoResto(SubtapFonte *nt, Fonte *F, const Fixt *fx) {
  Subtap *t = comeco(nt, F, FAIXA_PGS);
  colhePasso(F, fx->f.n, 5, 1);
  janelaAumenta(F, fx->f.n);       // revela TUDO: o fecho do C5 precisa do fim
  conferir(0, 1.25, 3.0, 1);
  conferir(1, 5.5, -1.0, 0);
  conferir(2, 9.7, 10.4, 1);
  conferir(3, 12.8, 13.3, 1);
  conferir(4, 24.3, -1.0, 1);
  { long gasto; TQ(!esperarEntregas(6, 600, &gasto), "entrega a mais (teve %d)", capturaN()); }
  SubtapStats st; subtap_estatisticas(t, &st);
  printf("fluxo: %ld bytes da janela de %ld do arquivo (%.1f%%)\n",
         st.bytes, (long)fx->f.n, 100.0 * (double)st.bytes / (double)fx->f.n);
  TQ(st.bytes > 0 && st.bytes * 2 < (long)fx->f.n,
     "custo alto demais: %ld de %ld bytes", st.bytes, (long)fx->f.n);
  TQ(st.blocos == 6 && st.entregues == 5 && st.descartados == 1,
     "contadores: blocos %ld entregues %ld descartados %ld", st.blocos, st.entregues, st.descartados);
  TQ(st.buracos == 0 && st.ressincronias == 1 && st.perdidos == 0,
     "resync: buracos %ld, ressincronias %ld, perdidos %ld", st.buracos, st.ressincronias, st.perdidos);
  // Depois do laceado, a cobertura publicada recomeca no C4 (18.0) e vai ate o
  // display set do C5 — os spans de antes NUNCA sao prometidos de novo.
  TQ(esperarCobertura(t, 18.0, 24.3, 2000), "cobertura final %s", "");
  subtap_parar(t);
}

// 2: seek no MEIO do payload de video; resync entrega so o depois dele.
static void cenarioSeek(SubtapFonte *nt, Fonte *F, const Fixt *fx) {
  Subtap *t = comeco(nt, F, FAIXA_PGS);
  colhePasso(F, fx->cl[5], 4, 1);
  TQ(capturaN() == 4, "pre-seek: %d entregas", capturaN());
  // O SEEK: geracao sobe, a janela pula PARA DENTRO do payload do video do
  // Cluster 4 (16 KiB antes do fim dele). Subir dele: so display set de
  // Cluster DEPOIS do resync pode sair — nem o limpa 18.2, nem retroceder.
  long t0 = agoraMs(), gasto;
  janelaSet(F, fx->videoMid[4], fx->videoMid[4] + JANELA);
  TQ(esperarEntregas(5, 2000, &gasto), "pos-seek sem entrega em %ld ms", gasto);
  TQ(agoraMs() - t0 <= 2000, "latencia do primeiro pos-seek: %ld ms", agoraMs() - t0);
  conferir(4, 24.3, -1.0, 1);        // 18.2 e 24.55 sao da faixa 2: skip
  janelaAumenta(F, fx->f.n);
  TQ(!esperarEntregas(6, 600, &gasto), "entrega a mais pos-seek (teve %d)", capturaN());
  SubtapStats st; subtap_estatisticas(t, &st);
  TQ(st.buracos == 1 && st.ressincronias == 2 && st.perdidos == 0,
     "resync pos-seek: buracos %ld, ressincronias %ld, perdidos %ld", st.buracos, st.ressincronias, st.perdidos);
  TQ(st.blocos == 6 && st.entregues == 5 && st.descartados == 1,
     "primeiro pos-seek: blocos %ld entregues %ld descartados %ld", st.blocos, st.entregues, st.descartados);
  TQ(esperarCobertura(t, 24.0, 24.3, 2000), "cobertura pos-seek %s", "");
  subtap_parar(t);
}

// 3: troca de faixa no meio do fluxo; cobertura zera, so a nova entrega.
static void cenarioTroca(SubtapFonte *nt, Fonte *F, const Fixt *fx) {
  Subtap *t = comeco(nt, F, FAIXA_PGS);
  colhePasso(F, fx->cl[5], 4, 1);
  janelaAumenta(F, fx->cl[5]); // C4 tem tamanho conhecido: fecha sem ler C5
  TQ(esperarCobertura(t, 18.0, 18.0, 2000), "cobertura antes da troca");
  subtap_faixa(t, FAIXA_PGS2);
  { // a cobertura ZERA na hora (o que saiu era da outra faixa)
    long t0 = agoraMs(); double d, a;
    while (subtap_cobertura(t, &d, &a)) {
      TQ(agoraMs() - t0 <= 1000, "cobertura nao zerou na troca de faixa");
      usleep(10000);
    }
  }
  colhePasso(F, fx->f.n, 5, 0);
  conferir(4, 24.55, -1.0, 0);       // so a faixa 2; 24.3 (faixa 6) vai skip
  { long gasto; TQ(!esperarEntregas(6, 600, &gasto), "entrega a mais na troca"); }
  SubtapStats st; subtap_estatisticas(t, &st);
  TQ(st.blocos == 6 && st.entregues == 5 && st.descartados == 1,
     "troca: blocos %ld entregues %ld descartados %ld", st.blocos, st.entregues, st.descartados);
  TQ(st.buracos == 0 && st.ressincronias == 2 && st.perdidos == 0,
     "troca: buracos %ld, ressincronias %ld, perdidos %ld", st.buracos, st.ressincronias, st.perdidos);
  TQ(esperarCobertura(t, 24.0, 24.55, 2000), "cobertura da nova faixa %s", "");
  subtap_parar(t);
}

// 4: bloco laceado da faixa alvo — descartado, e a cobertura do Cluster dele
//    NAO vale (num arquivo pequeno, de cobertura exata).
static void cenarioQuebra(SubtapFonte *nt, Fonte *F, const Fixt *fx) {
  Subtap *t = comeco(nt, F, FAIXA_PGS);
  colhePasso(F, fx->cl[1], 1, 1);    // Cluster A: entregue e fechado
  conferir(0, 1.25, 3.0, 1);
  TQ(esperarCobertura(t, 1.0, 1.25, 2000), "cobertura do Cluster A %s", "");
  colhePasso(F, fx->f.n, 2, 0);      // B (laceado) e C (limpa 9.5)
  conferir(1, 9.5, -1.0, 0);
  TQ(esperarCobertura(t, 9.0, 9.5, 2000),
     "cobertura depois do laceado: deveria RECOMECAr no Cluster C, nao [1.0, 9.5]");
  SubtapStats st; subtap_estatisticas(t, &st);
  TQ(st.blocos == 3 && st.entregues == 2 && st.descartados == 1,
     "pequenas: blocos %ld entregues %ld descartados %ld", st.blocos, st.entregues, st.descartados);
  TQ(st.buracos == 0 && st.ressincronias == 1 && st.perdidos == 0,
     "pequenas: buracos %ld, ressincronias %ld, perdidos %ld", st.buracos, st.ressincronias, st.perdidos);
  subtap_parar(t);
}

// 5: fonte encerrada com o fio vivo (o video fechou primeiro).
static void cenarioEncerraComFioVivo(void) {
  static Fonte F;
  memset(&F, 0, sizeof F);
  pthread_mutex_init(&F.trava, NULL);
  janelaSet(&F, 0, 0);               // janela vazia: o fio dorme
  capturaZera();
  SubtapFonte *nt = subtap_fonte_nova(&fOps, &F);
  TQ(nt != NULL, "fonte nao subiu");
  Subtap *t = subtap_iniciar(nt, FAIXA_PGS, 1000000UL, seguir, NULL);
  TQ(t != NULL, "fio nao subiu");
  usleep(200000);
  subtap_fonte_encerrar(nt);         // o video fechou com o fio da legenda vivo
  long t0 = agoraMs();
  subtap_parar(t);
  TQ(agoraMs() - t0 < 3000, "parar com fonte encerrada demorou %ld ms", agoraMs() - t0);
  TQ(capturaN() == 0, "nada deveria ter sido entregue");
  subtap_fonte_soltar(nt);           // a ultima referencia solta o ctx
  pthread_mutex_destroy(&F.trava);
}

// 6: parar enquanto o fio dorme esperando dado da janela.
static void cenarioPararDormindo(void) {
  static Fonte F;
  memset(&F, 0, sizeof F);
  pthread_mutex_init(&F.trava, NULL);
  janelaSet(&F, 0, 0);
  capturaZera();
  SubtapFonte *nt = subtap_fonte_nova(&fOps, &F);
  Subtap *t = subtap_iniciar(nt, FAIXA_PGS, 1000000UL, seguir, NULL);
  TQ(t != NULL, "fio nao subiu");
  usleep(200000);
  long t0 = agoraMs();
  subtap_parar(t);
  TQ(agoraMs() - t0 < 3000, "parar dormindo demorou %ld ms", agoraMs() - t0);
  subtap_fonte_soltar(nt);
  pthread_mutex_destroy(&F.trava);
}

// --- o teste -----------------------------------------------------------------

int main(void) {
  Fixt fixA, fixB;
  Fonte *FA, *FB;
  SubtapFonte *ntA, *ntB;

  montaF(&fixA, 6, tsA, subsA, subsPri, 2, 200 * 1024);
  montaF(&fixB, 3, tsB, subsB, NULL, -1, 4096);
  printf("arquivo grande: %ld bytes; pequeno: %ld bytes\n", (long)fixA.f.n, (long)fixB.f.n);

  ntA = fonteNovaSub(&FA, fixA.f.p, (int64_t)fixA.f.n);
  ntB = fonteNovaSub(&FB, fixB.f.p, (int64_t)fixB.f.n);
  TQ(ntA && ntB, "fontes nao subiram");

  cenarioFluxoResto(ntA, FA, &fixA);
  puts("1 fluxo inteiro: entregas, custo e cobertura ok");
  cenarioSeek(ntA, FA, &fixA);
  puts("2 seek no meio do video: resync e depois dele ok");

  cenarioTroca(ntA, FA, &fixA);
  puts("3 troca de faixa: cobertura zera e so a nova entrega ok");

  cenarioQuebra(ntB, FB, &fixB);
  puts("4 laceado descartado: cobertura do Cluster dele nao vale ok");

  cenarioEncerraComFioVivo();
  puts("5 fonte encerrada com o fio vivo: parar responde ok");

  cenarioPararDormindo();
  puts("6 parar dormindo: responde ok");

  subtap_fonte_soltar(ntA);
  subtap_fonte_soltar(ntB);
  pthread_mutex_destroy(&FA->trava); free(FA);
  pthread_mutex_destroy(&FB->trava); free(FB);
  free(fixA.f.p); free(fixB.f.p);
  puts("subtap: 6 cenarios ok");
  return 0;
}
