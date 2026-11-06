// Ver subtap.h. Um fio proprio segue a janela do anel de leitura do video e
// extrai os display sets PGS com o MESMO pgs.c do caminho por HTTP Range.
//
// INVARIANTES QUE O VIDEO DEPENDE:
//   - a janela so e LIDA (peek), nunca consumida: o cursor do player nao muda;
//   - nenhuma chamada espera por dado. Sem bytes na janela, o fio dorme;
//   - a trava do anel fica presa so pelo memcpy de um lote (<= 64 KiB);
//   - a memoria do parser e limitada por ST_TETO (4 MiB), que e o teto de UM
//     bloco de legenda; o que passar disso e pulado e derruba a cobertura
//     daquele Cluster, para o coletor por Range cobrir o trecho.
#include "subtap.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- parametros --------------------------------------------------------------

// Bytes por leitura da janela. Grande o bastante para nao pedir a trava do anel
// a cada bloco, pequeno o bastante para o memcpy nao atrasar o video.
#define ST_LOTE       (64L * 1024)
// Teto de UM bloco de legenda. Um display set 1080p com RLE fica em dezenas de
// KB; 4 MiB e o mesmo teto que pgs.c ja impoe a um objeto. Bloco maior que isto
// e pulado: nao vale segurar 4 MiB na TV por um caso que nao existe.
#define ST_TETO       (4L * 1024 * 1024)
// Profundidade de containers que o parser desce (Segment > Cluster > BlockGroup).
#define ST_NIVEL      8
// Ciclos de espera. Com a janela cheia de bytes novos o fio nao dorme; quando
// alcanca o produtor ele espera, e e essa espera que o deixa barato.
#define ST_SONO_MS        40
#define ST_SONO_OCIOSO_MS 150

#define ID_SEGMENT    0x18538067UL
#define ID_CLUSTER    0x1F43B675UL
#define ID_TIMESTAMP  0xE7UL
#define ID_SIMPLE     0xA3UL
#define ID_GROUP      0xA0UL
#define ID_BLOCK      0xA1UL
#define ID_DURATION   0x9BUL
#define ID_VOID       0xECUL
#define ID_CRC        0xBFUL

// --- fonte de bytes ----------------------------------------------------------

struct SubtapFonte {
  SubtapFonteOps op;
  void *ctx;
  int ativo;
  int refs;
  pthread_mutex_t trava;
};

SubtapFonte *subtap_fonte_nova(const SubtapFonteOps *ops, void *ctx) {
  SubtapFonte *f;
  if (!ops || !ops->janela || !ops->ler) return NULL;
  f = calloc(1, sizeof *f);
  if (!f) return NULL;
  f->op = *ops;
  f->ctx = ctx;
  f->ativo = 1;
  f->refs = 1;
  pthread_mutex_init(&f->trava, NULL);
  return f;
}

void subtap_fonte_reter(SubtapFonte *f) {
  if (!f) return;
  pthread_mutex_lock(&f->trava);
  f->refs++;
  pthread_mutex_unlock(&f->trava);
}

void subtap_fonte_soltar(SubtapFonte *f) {
  int ultima;
  if (!f) return;
  pthread_mutex_lock(&f->trava);
  ultima = --f->refs <= 0;
  pthread_mutex_unlock(&f->trava);
  if (!ultima) return;
  if (f->ativo && f->op.encerrar) f->op.encerrar(f->ctx);
  if (f->op.liberar) f->op.liberar(f->ctx);
  pthread_mutex_destroy(&f->trava);
  free(f);
}

int subtap_fonte_ativa(SubtapFonte *f) {
  int v;
  if (!f) return 0;
  pthread_mutex_lock(&f->trava);
  v = f->ativo;
  pthread_mutex_unlock(&f->trava);
  return v;
}

void subtap_fonte_encerrar(SubtapFonte *f) {
  if (!f) return;
  pthread_mutex_lock(&f->trava);
  if (f->ativo) {
    f->ativo = 0;
    /* Fora da trava da fonte: `encerrar` do dono pode querer a dele. */
    pthread_mutex_unlock(&f->trava);
    if (f->op.encerrar) f->op.encerrar(f->ctx);
    return;
  }
  pthread_mutex_unlock(&f->trava);
}

// --- estado ------------------------------------------------------------------

// `fim` e um offset ABSOLUTO exclusivo; -1 = tamanho desconhecido (o container
// so fecha quando aparece um ID de nivel 1, o que vale para Cluster).
typedef struct { unsigned long id; int64_t fim; } Nivel;

struct Subtap {
  SubtapFonte *fonte;
  SubtapEntrega entrega;
  void *ctx;
  pthread_t fio;
  int fioOk;
  int parar;

  /* Cobertura e faixa: escritas pelo fio, lidas pelo coletor por Range. */
  pthread_mutex_t trava;
  int faixa;
  unsigned long escala;
  int temDe, cobValida;
  double de, ate;
  int troca;             /* pedido de troca de faixa (o fio aplica) */

  /* Espera interrompivel (trava propria: a cobertura nao pode esperar por ela). */
  pthread_mutex_t sono;
  pthread_cond_t sinal;

  /* Buffer do parser. `pos`/`fim` sao indices; o offset do arquivo em `pos` e
   * ini + pos. So o fio mexe. */
  unsigned char *buf;
  long cap, pos, fim;
  int64_t ini;

  Nivel niv[ST_NIVEL];
  int nNiv;
  int ressinc;
  uint64_t geracao;
  int temGeracao;

  /* Cluster corrente. */
  unsigned long tempoCluster;
  double maxBloco;
  int temBloco;
  int quebra;            /* bloco da faixa alvo que o parser nao pode usar */

  /* BlockGroup: o Block espera o BlockDuration para saber o fim. */
  unsigned char *pend;
  long pendN, pendCap;
  double pendIni, pendFim;

  Pgs *pgs;

  /* Contadores (so o fio escreve; atomicos para a leitura de fora). */
  long blocos, entregues, descartados, bytes, buracos, ressincronias, perdidos;
};

// --- EBML --------------------------------------------------------------------

static unsigned long lerUint(const unsigned char *p, long n) {
  unsigned long v = 0;
  long i;
  for (i = 0; i < n && i < 8; i++) v = (v << 8) | p[i];
  return v;
}

// ID EBML (1..4 bytes, com o bit marcador preservado: e o valor que o Matroska
// usa nos numeros magicos).
static int lerId(const unsigned char *p, long n, unsigned long *id, int *len) {
  int k;
  unsigned long v = 0;
  if (n < 1) return 0;
  if (p[0] & 0x80) k = 1;
  else if (p[0] & 0x40) k = 2;
  else if (p[0] & 0x20) k = 3;
  else if (p[0] & 0x10) k = 4;
  else return 0;
  if (n < k) return 0;
  for (int i = 0; i < k; i++) v = (v << 8) | p[i];
  *id = v;
  *len = k;
  return 1;
}

// Tamanho EBML (vint de 1..8 bytes). `desconhecido`: todos os bits de dados em
// 1, o "sem tamanho" do Matroska.
static int lerTam(const unsigned char *p, long n, int64_t *v, int *len, int *desconhecido) {
  int k = 1, i, todos = 1;
  uint64_t x;
  if (n < 1) return 0;
  while (k <= 8 && !(p[0] & (0x80 >> (k - 1)))) k++;
  if (k > 8 || n < k) return 0;
  x = p[0] & (uint64_t)(0xFFu >> k);
  if (x != (uint64_t)(0xFFu >> k)) todos = 0;
  for (i = 1; i < k; i++) {
    x = (x << 8) | p[i];
    if (p[i] != 0xFF) todos = 0;
  }
  *v = (int64_t)x;
  *len = k;
  *desconhecido = todos;
  return 1;
}

// Vint de valor (TrackNumber de um bloco): igual ao tamanho, mas sem o caso
// "todos 1s".
static int lerVint(const unsigned char *p, long n, uint64_t *v, int *len) {
  int k = 1, i;
  uint64_t x;
  if (n < 1) return 0;
  while (k <= 8 && !(p[0] & (0x80 >> (k - 1)))) k++;
  if (k > 8 || n < k) return 0;
  x = p[0] & (uint64_t)(0xFFu >> k);
  for (i = 1; i < k; i++) x = (x << 8) | p[i];
  *v = x;
  *len = k;
  return 1;
}

// Elementos de nivel 1 do Segment. Nenhum filho de Cluster usa esses IDs, entao
// ve-los dentro de um Cluster de tamanho desconhecido significa que ele acabou.
static int ehNivel1(unsigned long id) {
  switch (id) {
    case ID_SEGMENT: case ID_CLUSTER: case 0x1C53BB6BUL /* Cues */:
    case 0x1043A770UL /* Chapters */: case 0x1254C367UL /* Tags */:
    case 0x1941A469UL /* Attachments */: case 0x114D9B74UL /* SeekHead */:
    case 0x1549A966UL /* Info */: case 0x1654AE6BUL /* Tracks */:
      return 1;
    default:
      return 0;
  }
}

// --- buffer ------------------------------------------------------------------

static int64_t stOff(const Subtap *t) { return t->ini + t->pos; }
static long stDisp(const Subtap *t) { return t->fim - t->pos; }

static void stCompacta(Subtap *t) {
  if (t->pos <= 0) return;
  memmove(t->buf, t->buf + t->pos, (size_t)(t->fim - t->pos));
  t->ini += t->pos;
  t->fim -= t->pos;
  t->pos = 0;
  /* Um bloco grande nao pode deixar 4 MiB presos pelo filme inteiro. */
  if (t->cap > 4 * ST_LOTE && t->fim < ST_LOTE) {
    unsigned char *g = realloc(t->buf, (size_t)(4 * ST_LOTE));
    if (g) { t->buf = g; t->cap = 4 * ST_LOTE; }
  }
}

static void stConsome(Subtap *t, long k) {
  t->pos += k;
  if (t->pos >= t->cap / 2) stCompacta(t);
}

// Pula k bytes sem le-los: e assim que o payload de video de um Cluster inteiro
// custa zero. Se o salto passa do que esta no buffer, o buffer esvazia e a
// proxima leitura comeca no destino.
static void stPula(Subtap *t, int64_t k) {
  long d = stDisp(t);
  if (k <= (int64_t)d) { stConsome(t, (long)k); return; }
  t->ini = stOff(t) + k;
  t->pos = t->fim = 0;
}

// Ha k bytes em [pos, fim)? Se nao, garante ESPACO para eles e devolve 0: quem
// chama pede mais bytes a janela (nunca completa o elemento com lixo).
static int stGarante(Subtap *t, long k) {
  long novo;
  unsigned char *g;
  if (k < 0) return 0;
  if (stDisp(t) >= k) return 1;
  if (t->cap - t->fim < k) {
    stCompacta(t);
    if (t->cap - t->fim < k) {
      novo = t->cap ? t->cap : 4 * ST_LOTE;
      while (novo - t->fim < k) novo *= 2;
      if (novo > ST_TETO + 4 * ST_LOTE) novo = ST_TETO + 4 * ST_LOTE;
      if (novo - t->fim < k) return 0;
      g = realloc(t->buf, (size_t)novo);
      if (!g) return 0;
      t->buf = g;
      t->cap = novo;
    }
  }
  return 0;
}

static void stResetaFluxo(Subtap *t) {
  pgs_liberar(t->pgs);
  t->pgs = pgs_novo(); // paletas e objetos nao atravessam seek nem troca
  t->nNiv = 0;
  t->pos = t->fim = 0;
  t->pendN = 0;
  t->quebra = 0;
  t->temBloco = 0;
  t->maxBloco = -1.0;
  t->tempoCluster = 0;
  t->ressinc = 1;
  pthread_mutex_lock(&t->trava);
  t->temDe = 0;
  t->cobValida = 0;
  pthread_mutex_unlock(&t->trava);
}

// --- entrega -----------------------------------------------------------------

static double stSegundos(const Subtap *t, int64_t unidades) {
  return (double)unidades * (double)t->escala / 1e9;
}

static void stEntrega(Subtap *t, const unsigned char *p, long n, double ini, double fim) {
  PgsQuadro q;
  int64_t lo, hi; uint64_t gen;
  if (!t->pgs || n <= 0) { t->quebra = 1; return; }
  if (!pgs_decodificar(t->pgs, p, (size_t)n, &q)) {
    t->quebra = 1;
    __atomic_add_fetch(&t->descartados, 1, __ATOMIC_RELAXED);
    return;
  }
  if (__atomic_load_n(&t->parar, __ATOMIC_RELAXED) ||
      __atomic_load_n(&t->troca, __ATOMIC_RELAXED) || !subtap_fonte_ativa(t->fonte) ||
      !t->fonte->op.janela(t->fonte->ctx, &lo, &hi, &gen) || gen != t->geracao || stOff(t) < lo) {
    pgs_quadro_liberar(&q); t->quebra = 1; return;
  }
  __atomic_add_fetch(&t->entregues, 1, __ATOMIC_RELAXED);
  t->entrega(t->ctx, ini, fim, &q);
}

static void stEntregaPendente(Subtap *t) {
  if (!t->pendN) return;
  stEntrega(t, t->pend, t->pendN, t->pendIni, t->pendFim);
  t->pendN = 0;
}

static int stGuardaPendente(Subtap *t, const unsigned char *p, long n, double ini) {
  if (n > ST_TETO) return 0;
  if (n > t->pendCap) {
    unsigned char *g = realloc(t->pend, (size_t)n);
    if (!g) return 0;
    t->pend = g;
    t->pendCap = n;
  }
  memcpy(t->pend, p, (size_t)n);
  t->pendN = n;
  t->pendIni = ini;
  t->pendFim = -1.0;
  return 1;
}

// --- containers --------------------------------------------------------------

static void stFechaCluster(Subtap *t) {
  double c = stSegundos(t, (int64_t)t->tempoCluster);
  pthread_mutex_lock(&t->trava);
  if (t->quebra) {
    /* Um bloco desta faixa ficou de fora: nada neste Cluster pode ser pulado
     * pelo coletor por Range. A cobertura volta a valer no proximo. */
    t->temDe = 0;
    t->cobValida = 0;
  } else {
    if (!t->temDe) {
      /* Primeiro Cluster depois de uma descontinuidade: `ate` recomeca aqui.
       * Sem isto a cobertura saltaria por cima do buraco (de novo, ate velho). */
      t->de = c;
      t->ate = c;
      t->temDe = 1;
    } else if (c > t->ate) {
      t->ate = c;
    }
    if (t->temBloco && t->maxBloco > t->ate) t->ate = t->maxBloco;
    t->cobValida = 1;
  }
  pthread_mutex_unlock(&t->trava);
}

static void stFechaNivel(Subtap *t) {
  unsigned long id;
  if (t->nNiv <= 0) return;
  id = t->niv[--t->nNiv].id;
  if (id == ID_GROUP) stEntregaPendente(t);
  else if (id == ID_CLUSTER) stFechaCluster(t);
}

static void stAbre(Subtap *t, unsigned long id, int64_t fim) {
  if (t->nNiv >= ST_NIVEL) return;
  t->niv[t->nNiv].id = id;
  t->niv[t->nNiv].fim = fim;
  t->nNiv++;
}

static int stNoCluster(const Subtap *t) {
  return t->nNiv > 0 && t->niv[t->nNiv - 1].id == ID_CLUSTER;
}

// --- blocos ------------------------------------------------------------------

// 0 = tratado; 1 = precisa de mais bytes em [ini+fim).
static int stBloco(Subtap *t, unsigned long id, long hdr, int64_t tam) {
  const unsigned char *p;
  long n, disp;
  int k, alvo = __atomic_load_n(&t->faixa, __ATOMIC_RELAXED);
  uint64_t faixa;
  int64_t rel;
  int flags, lacing;
  double ini;

  if (tam <= 0 || hdr + tam < hdr) {
    /* Tamanho insano: fluxo quebrou, nada aqui vale a promessa. */
    t->quebra = 1;
    __atomic_add_fetch(&t->descartados, 1, __ATOMIC_RELAXED);
    stPula(t, hdr + (tam > 0 ? tam : 0));
    return 0;
  }
  /* A faixa do bloco vem na CABECA: ler so ela antes do carregar payload e o
   * que faz o video (centenas de KB por Cluster) custar ZERO da janela — e o
   * que faz um bloco GRANDE da faixa ERRADA nao derrubar a cobertura (so um
   * bloco da NOSSA faixa grande demais quebra a promessa do Cluster). */
  {
    long cab = tam < 8 ? (long)tam : 8;
    if (!stGarante(t, hdr + cab)) return 1;
    p = t->buf + t->pos + hdr;
    if (!lerVint(p, cab, &faixa, &k)) {
      t->quebra = 1;
      __atomic_add_fetch(&t->descartados, 1, __ATOMIC_RELAXED);
      stPula(t, hdr + tam);
      return 0;
    }
    if (faixa != (uint64_t)alvo) { stPula(t, hdr + tam); return 0; }
  }
  if (tam > ST_TETO) {
    __atomic_add_fetch(&t->blocos, 1, __ATOMIC_RELAXED);
    /* Bloco da faixa alvo maior que o teto: nao da para usar nem para prometer
     * cobertura; o Cluster dele nao vale (a cobertura volta no proximo). */
    t->quebra = 1;
    __atomic_add_fetch(&t->descartados, 1, __ATOMIC_RELAXED);
    stPula(t, hdr + tam);
    return 0;
  }
  n = (long)tam;
  if (n < k + 3) {
    t->quebra = 1;
    __atomic_add_fetch(&t->descartados, 1, __ATOMIC_RELAXED);
    stPula(t, hdr + n); return 0;
  }
  if (!stGarante(t, hdr + n)) return 1;
  __atomic_add_fetch(&t->blocos, 1, __ATOMIC_RELAXED);
  p = t->buf + t->pos + hdr;
  /* Depois do stGarante pode ter compactado: rele vint/rel/flags o ponteiro
   * recem-feito (os primeiros bytes ja validados nao mudam). */
  if (!lerVint(p, n, &faixa, &k)) {
    t->quebra = 1;
    __atomic_add_fetch(&t->descartados, 1, __ATOMIC_RELAXED);
    stConsome(t, hdr + n);
    return 0;
  }
  rel = (int16_t)((p[k] << 8) | p[k + 1]);
  flags = p[k + 2];
  lacing = (flags >> 1) & 0x03;
  if (lacing) {
    /* PGS nunca e laced; se aparecer, o parser nao sabe separar as molduras. */
    t->quebra = 1;
    __atomic_add_fetch(&t->descartados, 1, __ATOMIC_RELAXED);
    stPula(t, hdr + n);
    return 0;
  }
  ini = stSegundos(t, (int64_t)t->tempoCluster + rel);
  if (ini > t->maxBloco) t->maxBloco = ini;
  t->temBloco = 1;
  disp = n - k - 3;
  if (id == ID_SIMPLE) {
    stEntrega(t, p + k + 3, disp, ini, -1.0);
  } else {
    stEntregaPendente(t);   /* um Block novo fecha o anterior */
    if (!stGuardaPendente(t, p + k + 3, disp, ini)) {
      t->quebra = 1;
      __atomic_add_fetch(&t->descartados, 1, __ATOMIC_RELAXED);
    }
  }
  stConsome(t, hdr + n);
  return 0;
}

// --- ressincronizacao --------------------------------------------------------

// Depois de um seek o anel recomeca no meio do arquivo, quase sempre dentro de
// um Cluster. Anda byte a byte ate um cabecalho de Cluster VALIDO: ID + tamanho
// + primeiro filho Timestamp. O custo e um Cluster (poucos MB), nao o filme.
static int stAchaCluster(Subtap *t) {
  for (;;) {
    long n, i;
    unsigned char *p;
    unsigned long id;
    int64_t tam;
    int idLen, tamLen, desconhecido;
    if (stDisp(t) < 4) return 1;
    p = t->buf + t->pos;
    n = stDisp(t);
    for (i = 0; i + 4 <= n; i++)
      if (p[i] == 0x1F && p[i + 1] == 0x43 && p[i + 2] == 0xB6 && p[i + 3] == 0x75) break;
    if (i + 4 > n) { stConsome(t, n - 3); return 1; }
    stConsome(t, i);
    /* ID de 4 bytes achado: o vint de tamanho tem comprimento no marcador,
     * peca so o necessario (a janela pode acabar logo depois do cabecalho). */
    if (stDisp(t) < 5) return 1;
    p = t->buf + t->pos;
    tamLen = 1;
    while (tamLen <= 8 && !(p[4] & (0x80 >> (tamLen - 1)))) tamLen++;
    if (tamLen > 8) { stConsome(t, 1); continue; }
    if (stDisp(t) < 4 + tamLen + 1) return 1;
    p = t->buf + t->pos;
    if (!lerId(p, stDisp(t), &id, &idLen) || id != ID_CLUSTER) { stConsome(t, 1); continue; }
    if (!lerTam(p + idLen, stDisp(t) - idLen, &tam, &tamLen, &desconhecido)) { stConsome(t, 1); continue; }
    /* O primeiro filho de um Cluster de verdade e o Timestamp. */
    if (p[idLen + tamLen] != 0xE7) { stConsome(t, 1); continue; }
    if (!desconhecido && (tam <= 0 || tam > ((int64_t)1 << 40))) { stConsome(t, 1); continue; }
    __atomic_add_fetch(&t->ressincronias, 1, __ATOMIC_RELAXED);
    t->ressinc = 0;
    return 0;
  }
}

// --- o parser ----------------------------------------------------------------

// 0 = consumiu/avancou; 1 = precisa de mais bytes em [ini+fim).
static int stProcessa(Subtap *t) {
  // Lotes limitados deixam o fio rever cancelamento, seek e troca de faixa.
  for (int passo = 0; passo < 256; passo++) {
    unsigned long id;
    int64_t tam;
    int idLen, tamLen, desconhecido;
    long hdr;
    const unsigned char *p;

    if (t->ressinc && stAchaCluster(t)) return 1;

    /* Fecha o que terminou. */
    while (t->nNiv > 0 && t->niv[t->nNiv - 1].fim >= 0 && stOff(t) >= t->niv[t->nNiv - 1].fim)
      stFechaNivel(t);

    if (stDisp(t) < 1) return 1;
    p = t->buf + t->pos;
    idLen = 1;
    while (idLen <= 4 && !(p[0] & (0x80 >> (idLen - 1)))) idLen++;
    if (idLen > 4) { t->quebra = 1; t->ressinc = 1; stConsome(t, 1); continue; }
    if (stDisp(t) < idLen) return 1;
    if (!lerId(p, stDisp(t), &id, &idLen)) { stConsome(t, 1); continue; }

    /* Cluster de tamanho desconhecido acaba quando comeca outro nivel 1. */
    if (t->nNiv > 0 && t->niv[t->nNiv - 1].fim < 0 && ehNivel1(id) &&
        t->niv[t->nNiv - 1].id != ID_SEGMENT) {
      stFechaNivel(t);
      continue;
    }

    /* O vint de tamanho tem comprimento proprio no primeiro byte: peca so o
     * necessario. Pedia 8 bytes "no escuro" antes — com a janela acabando em
     * um Cluster inteiro, o ULTIMO elemento (7 bytes, exato) nunca parseia e
     * o fecho dele trava para sempre. */
    if (stDisp(t) < idLen + 1) return 1;
    p = t->buf + t->pos;
    tamLen = 1;
    while (tamLen <= 8 && !(p[idLen] & (0x80 >> (tamLen - 1)))) tamLen++;
    if (tamLen > 8) { t->ressinc = 1; stConsome(t, 1); continue; }
    if (stDisp(t) < idLen + tamLen) return 1;
    if (!lerTam(p + idLen, stDisp(t) - idLen, &tam, &tamLen, &desconhecido)) {
      stConsome(t, 1);
      continue;
    }
    hdr = idLen + tamLen;
    if (!desconhecido && (tam < 0 || tam > ((int64_t)1 << 40))) {
      /* Tamanho absurdo: fluxo corrompido, volta a procurar um Cluster. */
      t->ressinc = 1;
      stConsome(t, 1);
      continue;
    }

    switch (id) {
      case ID_CLUSTER:
        stAbre(t, ID_CLUSTER, desconhecido ? -1 : stOff(t) + hdr + tam);
        t->tempoCluster = 0;
        t->temBloco = 0;
        t->maxBloco = -1.0;
        t->quebra = 0;
        stConsome(t, hdr);
        break;
      case ID_GROUP:
        stAbre(t, ID_GROUP, desconhecido ? -1 : stOff(t) + hdr + tam);
        stConsome(t, hdr);
        break;
      case ID_SEGMENT:
        stAbre(t, ID_SEGMENT, desconhecido ? -1 : stOff(t) + hdr + tam);
        stConsome(t, hdr);
        break;
      case ID_TIMESTAMP:
        if (desconhecido || tam > 8) { stPula(t, hdr + (desconhecido ? 0 : tam)); break; }
        if (!stGarante(t, hdr + (long)tam)) return 1;
        t->tempoCluster = lerUint(t->buf + t->pos + hdr, (long)tam);
        stConsome(t, hdr + (long)tam);
        break;
      case ID_SIMPLE:
      case ID_BLOCK:
        if (desconhecido) { t->ressinc = 1; stConsome(t, hdr); break; }
        if (stBloco(t, id, hdr, tam)) return 1;
        break;
      case ID_DURATION:
        if (desconhecido || tam > 8) { stPula(t, hdr + (desconhecido ? 0 : tam)); break; }
        if (!stGarante(t, hdr + (long)tam)) return 1;
        if (t->pendN) {
          unsigned long v = lerUint(t->buf + t->pos + hdr, (long)tam);
          t->pendFim = t->pendIni + stSegundos(t, (int64_t)v);
        }
        stConsome(t, hdr + (long)tam);
        break;
      default:
        if (desconhecido) { t->ressinc = 1; stConsome(t, hdr); break; }
        stPula(t, hdr + tam);
        break;
    }
  }
  return 0;
}

// --- o fio -------------------------------------------------------------------

static void stDorme(Subtap *t, long ms) {
  struct timespec rt;
  if (__atomic_load_n(&t->parar, __ATOMIC_RELAXED)) return;
  clock_gettime(CLOCK_REALTIME, &rt);
  rt.tv_nsec += ms * 1000000L;
  if (rt.tv_nsec >= 1000000000L) { rt.tv_sec++; rt.tv_nsec -= 1000000000L; }
  pthread_mutex_lock(&t->sono);
  if (!__atomic_load_n(&t->parar, __ATOMIC_RELAXED))
    pthread_cond_timedwait(&t->sinal, &t->sono, &rt);
  pthread_mutex_unlock(&t->sono);
}

// Le mais um lote da janela. Devolve 0 quando o byte ainda nao esta na memoria
// do leitor — o fio dorme e tenta de novo, sem pedir rede.
static int stCarrega(Subtap *t, int64_t lo, int64_t hi) {
  int64_t off = t->ini + t->fim;
  long quer;
  int got;
  if (off < lo || off >= hi) return 0;
  quer = (long)(hi - off);
  if (quer > ST_LOTE) quer = ST_LOTE;
  if (t->cap - t->fim < quer) {
    long novo;
    unsigned char *g;
    stCompacta(t);
    if (t->cap - t->fim < quer) {
      novo = t->cap ? t->cap * 2 : 4 * ST_LOTE;
      if (novo < t->fim + quer) novo = t->fim + quer;
      if (novo > ST_TETO + 4 * ST_LOTE) novo = ST_TETO + 4 * ST_LOTE;
      if (novo < t->fim + quer) return 0;
      g = realloc(t->buf, (size_t)novo);
      if (!g) return 0;
      t->buf = g;
      t->cap = novo;
    }
  }
  got = t->fonte->op.ler(t->fonte->ctx, off, t->buf + t->fim, (int)quer);
  if (got <= 0) return 0;
  t->fim += got;
  __atomic_add_fetch(&t->bytes, got, __ATOMIC_RELAXED);
  return 1;
}

static void stPrioridadeBaixa(void) {
#if defined(__linux__) && defined(SCHED_IDLE)
  /* Best-effort: o video manda. Se o sistema recusar, o sono do laco basta. */
  struct sched_param p;
  memset(&p, 0, sizeof p);
  pthread_setschedparam(pthread_self(), SCHED_IDLE, &p);
#endif
}

static void *stFio(void *arg) {
  Subtap *t = arg;
  stPrioridadeBaixa();
  for (;;) {
    int64_t lo = 0, hi = 0;
    uint64_t gen = 0;
    if (__atomic_load_n(&t->parar, __ATOMIC_RELAXED)) break;
    /* O dono encerrou a fonte (fechou o video): nao tocar mais nela. */
    if (!subtap_fonte_ativa(t->fonte)) break;
    if (__atomic_load_n(&t->troca, __ATOMIC_RELAXED)) {
      /* Outra faixa: o parser recomeca do zero no proximo Cluster. */
      __atomic_store_n(&t->troca, 0, __ATOMIC_RELAXED);
      stResetaFluxo(t);
    }
    if (!t->fonte->op.janela(t->fonte->ctx, &lo, &hi, &gen) || hi <= lo) {
      stDorme(t, ST_SONO_OCIOSO_MS);
      continue;
    }
    if (!t->temGeracao || gen != t->geracao || stOff(t) < lo) {
      /* Descontinuidade: seek do player, nova leitura, ou o anel passou por
       * cima do cursor. O que veio antes nao serve mais como cobertura. */
      if (t->temGeracao) {
        __atomic_add_fetch(&t->buracos, 1, __ATOMIC_RELAXED);
        if (t->temBloco && stNoCluster(t))
          __atomic_add_fetch(&t->perdidos, 1, __ATOMIC_RELAXED);
      }
      t->geracao = gen;
      t->temGeracao = 1;
      t->ini = lo;
      stResetaFluxo(t);
    }
    if (stProcessa(t) == 1 && !stCarrega(t, lo, hi)) stDorme(t, ST_SONO_MS);
  }
  return NULL;
}
 
// --- API ---------------------------------------------------------------------

Subtap *subtap_iniciar(SubtapFonte *fonte, int faixa, unsigned long escala,
                       SubtapEntrega entrega, void *ctx) {
  Subtap *t;
  if (!fonte || faixa <= 0 || !entrega) return NULL;
  t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->fonte = fonte;
  subtap_fonte_reter(fonte);
  t->entrega = entrega;
  t->ctx = ctx;
  __atomic_store_n(&t->faixa, faixa, __ATOMIC_RELAXED);
  t->escala = escala ? escala : 1000000UL;
  t->maxBloco = -1.0;
  t->ressinc = 1;
  t->pgs = pgs_novo();
  if (!t->pgs) { subtap_fonte_soltar(fonte); free(t); return NULL; }
  pthread_mutex_init(&t->trava, NULL);
  pthread_mutex_init(&t->sono, NULL);
  pthread_cond_init(&t->sinal, NULL);
  if (pthread_create(&t->fio, NULL, stFio, t)) {
    pgs_liberar(t->pgs);
    subtap_fonte_soltar(fonte);
    pthread_cond_destroy(&t->sinal);
    pthread_mutex_destroy(&t->sono);
    pthread_mutex_destroy(&t->trava);
    free(t);
    return NULL;
  }
  t->fioOk = 1;
  return t;
}

void subtap_faixa(Subtap *t, int faixa) {
  if (!t) return;
  pthread_mutex_lock(&t->trava);
  __atomic_store_n(&t->faixa, faixa > 0 ? faixa : 0, __ATOMIC_RELAXED);
  /* Outra faixa: o que ja foi entregue e de outro TrackNumber. */
  t->temDe = 0;
  t->cobValida = 0;
  pthread_mutex_unlock(&t->trava);
  /* Quem zera o resto do parser e o fio, no proprio contexto. */
  __atomic_store_n(&t->troca, 1, __ATOMIC_RELAXED);
}

int subtap_cobertura(Subtap *t, double *de, double *ate) {
  int ok;
  if (!t) return 0;
  pthread_mutex_lock(&t->trava);
  ok = t->cobValida && t->temDe && t->ate >= t->de;
  if (ok) { if (de) *de = t->de; if (ate) *ate = t->ate; }
  pthread_mutex_unlock(&t->trava);
  return ok;
}

void subtap_estatisticas(Subtap *t, SubtapStats *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!t) return;
  out->blocos = __atomic_load_n(&t->blocos, __ATOMIC_RELAXED);
  out->entregues = __atomic_load_n(&t->entregues, __ATOMIC_RELAXED);
  out->descartados = __atomic_load_n(&t->descartados, __ATOMIC_RELAXED);
  out->bytes = __atomic_load_n(&t->bytes, __ATOMIC_RELAXED);
  out->buracos = __atomic_load_n(&t->buracos, __ATOMIC_RELAXED);
  out->ressincronias = __atomic_load_n(&t->ressincronias, __ATOMIC_RELAXED);
  out->perdidos = __atomic_load_n(&t->perdidos, __ATOMIC_RELAXED);
  pthread_mutex_lock(&t->trava);
  out->faixa = __atomic_load_n(&t->faixa, __ATOMIC_RELAXED);
  out->de = t->temDe ? t->de : 0.0;
  out->ate = t->cobValida ? t->ate : 0.0;
  pthread_mutex_unlock(&t->trava);
}

void subtap_parar(Subtap *t) {
  if (!t) return;
  __atomic_store_n(&t->parar, 1, __ATOMIC_RELAXED);
  pthread_mutex_lock(&t->sono);
  pthread_cond_broadcast(&t->sinal);
  pthread_mutex_unlock(&t->sono);
  if (t->fioOk) pthread_join(t->fio, NULL);
  t->fioOk = 0;
  pgs_liberar(t->pgs);
  subtap_fonte_soltar(t->fonte);
  free(t->buf);
  free(t->pend);
  pthread_cond_destroy(&t->sinal);
  pthread_mutex_destroy(&t->sono);
  pthread_mutex_destroy(&t->trava);
  free(t);
}
