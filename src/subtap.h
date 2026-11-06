// Legenda de imagem (S_HDMV/PGS) colhida dos bytes que JA passam pelo buffer de
// leitura do video, em vez de um segundo conjunto de HTTP Range.
//
// O QUE JA EXISTE. Em webOS o video nao le a URL do add-on: le um servidor
// loopback do moviebuffer (src/moviebuffer.c), que por sua vez le de um anel de
// leitura antecipada do dts_engine (fx_buf, 64/128/256 MiB). Esse anel guarda
// bytes do arquivo em offsets ABSOLUTOS e contiguos: [fx_lo, fx_hi). Ele fica
// ~FX_CAP-FX_HIST a frente do playhead — e a mesma leitura antecipada que o
// player consome.
//
// O QUE FALTAVA. O coletor de legenda (mkvass.c) baixava de novo, por Range,
// exatamente os blocos PGS que o anel ja tinha. Este modulo observa o anel e
// extrai os display sets SEM tocar na rede.
//
// COMO. Um fio proprio segue o cursor do anel (nunca o consome: so copia), roda
// um parser Matroska incremental e entrega os display sets decodificados pelo
// mesmo pgs.c do caminho por Range. Se o anel nao cobre o byte que o parser
// precisa, o fio DORME — nao bloqueia produtor nem consumidor do video, nao
// pede rede e nao inventa dados.
//
// PROTOTIPO: nao e iniciado por mkvass/moviebuffer e nao substitui Range.
// A cobertura do anel e
// parcial por construcao (ele comeca onde o video esta e avanca com ele), entao
// `subtap_cobertura` publica so o intervalo de tempo REALMENTE percorrido, e o
// nao prova que imgsub ainda guarda os quadros. Nao use esse intervalo para
// marcar pontos como colhidos permanentemente. Ver o plano em docs/plans/.
#ifndef NV_SUBTAP_H
#define NV_SUBTAP_H
#include <stdint.h>
#include "pgs.h"

// --- fonte de bytes ----------------------------------------------------------
//
// Uma janela contigua [lo, hi) do arquivo que esta na memoria do leitor, mais
// a copia de um trecho dela. As duas chamadas NAO bloqueiam e NAO alteram o
// cursor de quem le o video: devolvem 0 quando o byte nao esta la.
//
// A fonte e refcontada porque o dono (moviebuffer) pode fechar o video enquanto
// o coletor ainda tem o fio vivo. `encerrar` e chamado pelo dono DEPOIS de parar
// o fio (subtap_parar): depois dele nenhuma chamada toca no leitor de novo. Se
// o dono so soltar a ultima referencia, `encerrar` roda na destruicao, tambem
// com o fio ja parado.
typedef struct SubtapFonte SubtapFonte;
typedef struct {
  int  (*janela)(void *ctx, int64_t *lo, int64_t *hi, uint64_t *geracao);
  int  (*ler)(void *ctx, int64_t off, unsigned char *dst, int n);
  void (*encerrar)(void *ctx);
  void (*liberar)(void *ctx);
} SubtapFonteOps;

SubtapFonte *subtap_fonte_nova(const SubtapFonteOps *ops, void *ctx);
void subtap_fonte_reter(SubtapFonte *f);
void subtap_fonte_soltar(SubtapFonte *f);
// 1 enquanto o dono nao a encerrou.
int  subtap_fonte_ativa(SubtapFonte *f);
// Marca a fonte como encerrada e chama o `encerrar` do dono. Idempotente.
void subtap_fonte_encerrar(SubtapFonte *f);

// --- coletor ----------------------------------------------------------------

// Um display set PGS decodificado. `ini`/`fim` em segundos do INICIO DO ARQUIVO
// (a mesma origem do currentTime do player). `fim < 0`: o proximo display set
// troca (ou um "limpar" de composicao vazia). A posse dos bitmaps passa ao
// chamador, que libera com pgs_quadro_liberar quando nao publicar.
typedef void (*SubtapEntrega)(void *ctx, double ini, double fim, PgsQuadro *q);

typedef struct Subtap Subtap;

typedef struct {
  long blocos;        // display sets da faixa alvo vistos no fluxo
  long entregues;     // display sets entregues (com composicao valida)
  long descartados;   // blocos da faixa que o parser nao conseguiu usar
  long bytes;         // bytes percorridos pelo parser
  long buracos;       // descontinuidades (seek, queda do anel) que pediram resync
  long ressincronias; // varreduras de Cluster ate achar um cabecalho valido
  long perdidos;      // trechos de Cluster abandonados no meio (podem ter blocos)
  int  faixa;         // TrackNumber alvo (0 = nenhuma)
  double de, ate;     // cobertura contigua em segundos (de > ate = vazia)
} SubtapStats;

// Comeca a seguir o anel. `faixa` e o TrackNumber do Matroska (o mesmo de
// VideoFaixa.numero) e `escala` o TimestampScale em ns por unidade (1e6 no
// padrao). Devolve NULL se nao houver fonte, faixa ou memoria. A fonte e
// retida: quem chama pode soltar a referencia dele logo depois.
Subtap *subtap_iniciar(SubtapFonte *fonte, int faixa, unsigned long escala,
                       SubtapEntrega entrega, void *ctx);
// Troca a faixa alvo sem parar o fio. Zera a cobertura: o que ja foi entregue
// era de outra faixa.
void subtap_faixa(Subtap *t, int faixa);
// Cobertura contigua em segundos, para quem colhe por Range pular o que este
// fio ja entregou. 0 = sem cobertura (nunca andou, ou acabou de ressincronizar).
int  subtap_cobertura(Subtap *t, double *de, double *ate);
void subtap_estatisticas(Subtap *t, SubtapStats *out);
// Para o fio, junta e libera o objeto. Chamar uma vez; NULL e permitido.
// Nao chamar de dentro de entrega. O callback deve retornar para o join acabar.
void subtap_parar(Subtap *t);

#endif
