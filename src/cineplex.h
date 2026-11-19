// Cineplex (ISP media server) as one more personal server, next to Jellyfin/Emby/Plex.
//
// WHAT IT DOES. Reads the server's own web pages (tvs.php, tcategory.php, watch.php), shows the
// WHOLE series library as Home rows, fills the title page (seasons / episodes) and hands the player
// the HLS link together with the cookies (hlse / hlsk) the server demands, plus User-Agent and
// Referer, as stream headers. The native player sends them, so no relay and no addon is needed and
// the title does not have to exist on TMDB/IMDb.
//
// IDS. "cx:<8 hex tag>.<hex>" like the other personal servers (jfid.h). A series is 8 hex digits
// (series_id). An episode is 16 hex digits: series (8) + season (4) + episode (4).
#ifndef NV_CINEPLEX_H
#define NV_CINEPLEX_H
#include "catalogo.h"
#include "streams.h"

#define CX_FIL_MAX 6
#define CX_POR_FIL 24
#define CX_ITENS_MAX (CX_FIL_MAX * CX_POR_FIL)

enum { CX_FONTES_NADA = 0, CX_FONTES_PENDENTE, CX_FONTES_PRONTO, CX_FONTES_FALHOU };

void cineplex_carregar(void);           // starts the (one-off) library read; cheap and idempotent
void cineplex_perfil_trocou(void);
void cineplex_esquecer_todos(void);     // drops the cookies
int  cineplex_ligado(void);             // 1 = the Cineplex rows are on
int  cineplex_conectado(void);          // 1 = the library was read and has rows
unsigned cineplex_fileiras_versao(void);
int  cineplex_chave_fileira(const char *chave);
int  cineplex_fileiras_copiar(CatItem *itens, int maxItens, CatFileira *fils, int maxFils, int *nItens);
int  cineplex_ficha(CatItem *item, CatEp *eps, int maxEps);
int  cineplex_fontes_pedir(const char *alvo);
int  cineplex_fontes_colher(const char *alvo, Stream **lista, int *n);
// "Ver tudo": page `pagina` (1-based) of a row. catId = "all" (every category in turn) or "c:<category>".
// BLOCKS (network). Returns the number of titles written to `saida` (0 .. max), or -1 on failure.
// *fim = 1 when this was the last page.
int  cineplex_pagina(const char *catId, int pagina, CatItem *saida, int max, int *fim);
// Search the server's own index (search_ajax.php). BLOCKS. Returns the number of titles written.
int  cineplex_buscar(const char *termo, CatItem *saida, int max);
void cineplex_reproducao_tick(const char *url, double posSeg, double durSeg, int tocando);
void cineplex_reproducao_fim(const char *url, double posSeg, double durSeg);

// Pure helpers (tests): id <-> series / season / episode.
int cineplex_id_serie(char *dst, size_t tam, unsigned long serie);
int cineplex_id_episodio(char *dst, size_t tam, unsigned long serie, int temporada, int episodio);
// 1 = valid. *temporada / *episodio are 0 for a series id.
int cineplex_id_partes(const char *id, unsigned long *serie, int *temporada, int *episodio);
#endif
