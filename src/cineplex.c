#include "cineplex.h"
#include "jfid.h"
#include "rede.h"
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef CX_BASE_PADRAO
#define CX_BASE_PADRAO "http://103.136.200.66"
#endif
#define CX_UA "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Mobile Safari/537.36"
#define CX_TAG "c1e9a000"
#define CX_PAGINA_MAX (3u * 1024u * 1024u)
#define CX_PRAZO_MS 15000u
#define CX_TEMPS_MAX 24
#define CX_EPS_PAGINA 400

static pthread_mutex_t trava = PTHREAD_MUTEX_INITIALIZER;
static char base[128];
static unsigned geracao = 1;

// ------------------------------------------------------------------ ids
int cineplex_id_serie(char *dst, size_t tam, unsigned long serie) {
  char hex[16];
  if (serie == 0 || serie > 0xfffffffful) return 0;
  snprintf(hex, sizeof hex, "%08lx", serie);
  return jfid_montar_p(JFID_PREFIXO_CX, dst, tam, CX_TAG, hex);
}
int cineplex_id_episodio(char *dst, size_t tam, unsigned long serie, int temporada, int episodio) {
  char hex[24];
  if (serie == 0 || serie > 0xfffffffful || temporada < 0 || temporada > 0xffff || episodio < 1 || episodio > 0xffff) return 0;
  snprintf(hex, sizeof hex, "%08lx%04x%04x", serie, (unsigned)temporada, (unsigned)episodio);
  return jfid_montar_p(JFID_PREFIXO_CX, dst, tam, CX_TAG, hex);
}
int cineplex_id_partes(const char *id, unsigned long *serie, int *temporada, int *episodio) {
  char tag[JFID_TAG + 1], item[JFID_ITEM + 1];
  size_t n;
  char b[9];
  if (serie) *serie = 0;
  if (temporada) *temporada = 0;
  if (episodio) *episodio = 0;
  if (!jfid_partes_p(JFID_PREFIXO_CX, id, tag, item)) return 0;
  if (strncasecmp(tag, CX_TAG, JFID_TAG)) return 0;
  n = strlen(item);
  if (n != 8 && n != 16) return 0;
  memcpy(b, item, 8); b[8] = 0;
  if (serie) *serie = strtoul(b, NULL, 16);
  if (n == 16) {
    char t[5], e[5];
    memcpy(t, item + 8, 4); t[4] = 0;
    memcpy(e, item + 12, 4); e[4] = 0;
    if (temporada) *temporada = (int)strtoul(t, NULL, 16);
    if (episodio) *episodio = (int)strtoul(e, NULL, 16);
  }
  return !serie || *serie != 0;
}

// -------------------------------------------------------------- strings
static void copiar(char *dst, size_t tam, const char *src) {
  if (!tam) return;
  snprintf(dst, tam, "%s", src ? src : "");
}
static const char *buscar(const char *s, const char *fim, const char *ag) {
  size_t n = strlen(ag);
  if (!s) return NULL;
  for (; s + n <= fim; s++)
    if (*s == *ag && !memcmp(s, ag, n)) return s;
  return NULL;
}
static void entidades(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (*r == '&') {
      if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; continue; }
      if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; continue; }
      if (!strncmp(r, "&#039;", 6) || !strncmp(r, "&#39;", 5)) { *w++ = '\''; r += r[3] == '3' && r[4] == '9' ? 5 : 6; continue; }
      if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; continue; }
      if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; continue; }
      if (!strncmp(r, "&nbsp;", 6)) { *w++ = ' '; r += 6; continue; }
    }
    *w++ = *r++;
  }
  *w = 0;
}
// Text between s and fim with tags removed and whitespace collapsed.
static void texto(const char *s, const char *fim, char *dst, size_t tam) {
  size_t k = 0;
  int tag = 0, esp = 1;
  if (!tam) return;
  for (; s && s < fim && k + 1 < tam; s++) {
    if (*s == '<') { tag = 1; continue; }
    if (*s == '>') { tag = 0; continue; }
    if (tag) continue;
    if (isspace((unsigned char)*s)) { if (!esp) { dst[k++] = ' '; esp = 1; } continue; }
    dst[k++] = *s; esp = 0;
  }
  while (k && dst[k - 1] == ' ') k--;
  dst[k] = 0;
  entidades(dst);
}
// attribute value (name must include `="`), searched in [s, fim).
static int atributo(const char *s, const char *fim, const char *nome, char *dst, size_t tam) {
  const char *p = buscar(s, fim, nome), *q;
  size_t n;
  if (!p) return 0;
  p += strlen(nome);
  q = memchr(p, '"', (size_t)(fim - p));
  if (!q) return 0;
  n = (size_t)(q - p);
  if (n >= tam) n = tam - 1;
  memcpy(dst, p, n); dst[n] = 0;
  entidades(dst);
  return 1;
}
static void absoluta(const char *u, char *dst, size_t tam) {
  if (!u || !*u || !strncmp(u, "data:", 5)) { if (tam) dst[0] = 0; return; }
  if (!strncasecmp(u, "http://", 7) || !strncasecmp(u, "https://", 8)) copiar(dst, tam, u);
  else if (u[0] == '/' && u[1] == '/') snprintf(dst, tam, "http:%s", u);
  else snprintf(dst, tam, "%s%s%s", base, u[0] == '/' ? "" : "/", u);
}

// -------------------------------------------------------------- cookies
static struct { char n[16]; char v[220]; } jar[6];
static int nJar;
static void guardarCookie(const char *nome, size_t nn, const char *val, size_t nv) {
  int i;
  if (nn == 0 || nn >= sizeof jar[0].n || nv == 0 || nv >= sizeof jar[0].v) return;
  if (!((nn == 4 && !strncasecmp(nome, "hlse", 4)) || (nn == 4 && !strncasecmp(nome, "hlsk", 4)) ||
        (nn == 8 && !strncasecmp(nome, "redforce", 8)))) return;
  pthread_mutex_lock(&trava);
  for (i = 0; i < nJar; i++)
    if (strlen(jar[i].n) == nn && !strncasecmp(jar[i].n, nome, nn)) break;
  if (i == nJar && nJar < 6) nJar++;
  if (i < nJar) {
    memcpy(jar[i].n, nome, nn); jar[i].n[nn] = 0;
    memcpy(jar[i].v, val, nv); jar[i].v[nv] = 0;
  }
  pthread_mutex_unlock(&trava);
}
static void colherCookies(const char *s, size_t n) {
  const char *p = s, *fim = s + n;
  static const char *nomes[] = { "hlse=", "hlsk=", "REDFORCE=", "redforce=" };
  int k;
  if (!s) return;
  for (k = 0; k < 4; k++) {
    p = s;
    while ((p = buscar(p, fim, nomes[k])) != NULL) {
      const char *v = p + strlen(nomes[k]), *e = v;
      if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) { p = v; continue; }
      while (e < fim && *e != ';' && *e != '"' && *e != '\'' && *e != '\r' && *e != '\n' && *e != ' ' && *e != '<') e++;
      guardarCookie(p, strlen(nomes[k]) - 1, v, (size_t)(e - v));
      p = e;
    }
  }
}
static void cookieCabecalho(char *dst, size_t tam) {
  int i;
  size_t k = 0;
  if (tam) dst[0] = 0;
  pthread_mutex_lock(&trava);
  for (i = 0; i < nJar; i++) {
    int w = snprintf(dst + k, tam - k, "%s%s=%s", k ? "; " : "", jar[i].n, jar[i].v);
    if (w < 0 || (size_t)w >= tam - k) { dst[k] = 0; break; }
    k += (size_t)w;
  }
  pthread_mutex_unlock(&trava);
}

// ----------------------------------------------------------------- HTTP
static void baseIniciar(void) {
  const char *e = getenv("NV_CX_BASE");
  pthread_mutex_lock(&trava);
  if (!base[0]) {
    copiar(base, sizeof base, e && *e ? e : CX_BASE_PADRAO);
    for (size_t n = strlen(base); n && base[n - 1] == '/'; n--) base[n - 1] = 0;
  }
  pthread_mutex_unlock(&trava);
}
// 1 = HTTP 2xx and a body (malloc'd, NUL-terminated; free()).
static int pegar(const char *caminho, char **corpo, size_t *n) {
  char url[800], ck[256], cabCk[300];
  const char *cab[4];
  RedePedido p;
  RedeResposta r;
  int ok = 0, k = 0;
  *corpo = NULL;
  if (n) *n = 0;
  baseIniciar();
  snprintf(url, sizeof url, "%s%s", base, caminho);
  cab[k++] = "User-Agent: " CX_UA;
  cab[k++] = "Accept: text/html,application/json,*/*";
  cookieCabecalho(ck, sizeof ck);
  if (ck[0]) { snprintf(cabCk, sizeof cabCk, "Cookie: %s", ck); cab[k++] = cabCk; }
  cab[k] = NULL;
  memset(&p, 0, sizeof p);
  memset(&r, 0, sizeof r);
  p.url = url;
  p.cabecalhos = cab;
  p.prazo_ms = CX_PRAZO_MS;
  p.max_bytes = CX_PAGINA_MAX;
  p.seguir = 1;
  p.tls_pessoal = 1;
  if (rede_pedir(&p, &r) && r.status >= 200 && r.status < 300 && r.corpo) {
    char *c = malloc(r.n_corpo + 1);
    colherCookies(r.cabecalhos, r.n_cabecalhos);
    if (c) {
      memcpy(c, r.corpo, r.n_corpo);
      c[r.n_corpo] = 0;
      colherCookies(c, r.n_corpo);
      *corpo = c;
      if (n) *n = r.n_corpo;
      ok = 1;
    }
  }
  rede_resposta_limpar(&r);
  return ok;
}

// ---------------------------------------------------------------- cards
typedef struct { unsigned long id; char titulo[160]; char poster[300]; char ano[8]; char cat[64]; int nota; } Cartao;

static int ler_cartoes(const char *html, Cartao *out, int max) {
  const char *fim = html + strlen(html), *p = html;
  int n = 0, i;
  while (n < max && (p = buscar(p, fim, "series_id=")) != NULL) {
    const char *a0 = p, *a1, *q;
    Cartao *c = &out[n];
    char tmp[320];
    unsigned long id = strtoul(p + 10, NULL, 10);
    while (a0 > html && !(a0[0] == '<' && (a0[1] == 'a' || a0[1] == 'A') && isspace((unsigned char)a0[2]))) a0--;
    a1 = buscar(p, fim, "</a>");
    if (!a1) a1 = fim;
    memset(c, 0, sizeof *c);
    c->id = id;
    if (!atributo(a0, a1, "alt=\"", c->titulo, sizeof c->titulo) || !c->titulo[0] || id == 0) { p = a1; continue; }
    for (i = 0; i < n; i++) if (out[i].id == id) break;
    if (i < n) { p = a1; continue; }
    tmp[0] = 0;
    if (!atributo(a0, a1, "data-src=\"", tmp, sizeof tmp) || !tmp[0]) atributo(a0, a1, " src=\"", tmp, sizeof tmp);
    absoluta(tmp, c->poster, sizeof c->poster);
    if ((q = buscar(a0, a1, "amber")) && (q = memchr(q, '>', (size_t)(a1 - q)))) {
      texto(q + 1, a1, tmp, sizeof tmp);
      tmp[4] = 0;
      if (strlen(tmp) == 4 && isdigit((unsigned char)tmp[0])) copiar(c->ano, sizeof c->ano, tmp);
    }
    if ((q = buscar(a0, a1, "text-yellow-400")) && (q = memchr(q, '>', (size_t)(a1 - q)))) {
      const char *d = q + 1;
      while (d < a1 && *d != '<' && !isdigit((unsigned char)*d)) {
        if (*d == '&') { const char *sc = memchr(d, ';', (size_t)(a1 - d)); if (sc && sc - d < 10) d = sc; }   // &#9733; (the star)
        d++;
      }
      if (d < a1 && isdigit((unsigned char)*d)) { double v = atof(d); if (v > 0 && v <= 10) c->nota = (int)(v * 10.0 + 0.5); }
    }
    if ((q = buscar(a0, a1, "catLabel")) && (q = memchr(q, '>', (size_t)(a1 - q)))) {
      const char *e = buscar(q, a1, "</");
      texto(q + 1, e ? e : a1, c->cat, sizeof c->cat);
    }
    n++;
    p = a1;
  }
  return n;
}

static void item_de(const Cartao *c, CatItem *it) {
  memset(it, 0, sizeof *it);
  copiar(it->tipo, sizeof it->tipo, "series");
  cineplex_id_serie(it->imdb, sizeof it->imdb, c->id);
  copiar(it->titulo, sizeof it->titulo, c->titulo);
  copiar(it->poster, sizeof it->poster, c->poster);
  copiar(it->meta, sizeof it->meta, c->ano);
  copiar(it->genero, sizeof it->genero, c->cat);
  it->nota = c->nota;
}

// ------------------------------------------------------------ snapshot
static CatFileira snapFils[CX_FIL_MAX];
static CatItem *snapItens;
static int nSnapFils;
static unsigned snapVersao;
static int carregando, carregado;
static unsigned snapGeracao;

static void decodificar_nome(const char *in, size_t n, char *out, size_t tam) {
  size_t k = 0, i;
  for (i = 0; i < n && k + 1 < tam; i++) {
    if (in[i] == '+') out[k++] = ' ';
    else if (in[i] == '%' && i + 2 < n && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
      char h[3] = { in[i + 1], in[i + 2], 0 };
      out[k++] = (char)strtol(h, NULL, 16); i += 2;
    } else out[k++] = in[i];
  }
  out[k] = 0;
}

#define CX_CATS_MAX 48
static char catEnc[CX_CATS_MAX][96];   // category names exactly as the site links them (url-encoded)
static int catPag[CX_CATS_MAX];        // number of pages of each category (0 = unknown)
static int nCatsReg;

// "Page 2 of 17" -> 17 (0 when absent)
static int total_paginas(const char *html) {
  const char *fim = html + strlen(html), *p = html;
  while ((p = buscar(p, fim, "age ")) != NULL) {
    const char *q = p + 4;
    if (p > html && (p[-1] == 'P' || p[-1] == 'p') && isdigit((unsigned char)*q)) {
      while (q < fim && isdigit((unsigned char)*q)) q++;
      while (q < fim && isspace((unsigned char)*q)) q++;
      if (!strncasecmp(q, "of", 2)) {
        q += 2;
        while (q < fim && isspace((unsigned char)*q)) q++;
        if (isdigit((unsigned char)*q)) return atoi(q);
      }
    }
    p += 4;
  }
  return 0;
}

static void *carregarFio(void *u) {
  unsigned g = (unsigned)(uintptr_t)u;
  char *html = NULL, caminho[160];
  char (*cats)[96] = malloc(sizeof(char[96]) * CX_CATS_MAX);
  int *pags = calloc(CX_CATS_MAX, sizeof(int));
  int nCats = 0, nf = 0, ni = 0, k;
  CatItem *itens = malloc(sizeof(CatItem) * CX_ITENS_MAX);
  CatFileira fils[CX_FIL_MAX];
  Cartao *cartoes = malloc(sizeof(Cartao) * CX_POR_FIL);
  if (!itens || !cartoes || !cats || !pags) goto fim;
  memset(fils, 0, sizeof fils);
  if (pegar("/tvs.php", &html, NULL)) {
    const char *p = html, *fimH = html + strlen(html);
    int n = ler_cartoes(html, cartoes, CX_POR_FIL);
    while (nCats < CX_CATS_MAX && (p = buscar(p, fimH, "tcategory.php?category=")) != NULL) {
      const char *v = p + 23, *e = v;
      int ja = 0;
      while (e < fimH && *e != '"' && *e != '&' && *e != '\'' && *e != ' ' && *e != '<' && *e != '>') e++;
      if (e > v && (size_t)(e - v) < sizeof cats[0]) {
        for (k = 0; k < nCats; k++) if (strlen(cats[k]) == (size_t)(e - v) && !strncmp(cats[k], v, (size_t)(e - v))) ja = 1;
        if (!ja) { memcpy(cats[nCats], v, (size_t)(e - v)); cats[nCats][e - v] = 0; nCats++; }
      }
      p = e;
    }
    if (n > 0) {
      for (k = 0; k < n; k++) item_de(&cartoes[k], &itens[ni + k]);
      snprintf(fils[nf].chave, sizeof fils[nf].chave, "cx_series_all");
      snprintf(fils[nf].titulo, sizeof fils[nf].titulo, "Cineplex · All series");
      copiar(fils[nf].tipo, sizeof fils[nf].tipo, "series");
      copiar(fils[nf].base, sizeof fils[nf].base, "cineplex:");
      copiar(fils[nf].catId, sizeof fils[nf].catId, "all");
      fils[nf].ini = ni; fils[nf].n = n;
      ni += n; nf++;
    }
    free(html); html = NULL;
  }
  // page 1 of EVERY category: fills the first rows and tells how many pages each one has
  for (k = 0; k < nCats; k++) {
    int n, j, stop;
    char nome[96];
    pthread_mutex_lock(&trava);
    stop = (g != geracao);
    pthread_mutex_unlock(&trava);
    if (stop) goto fim;
    snprintf(caminho, sizeof caminho, "/tcategory.php?category=%s&page=1", cats[k]);
    if (!pegar(caminho, &html, NULL)) continue;
    pags[k] = total_paginas(html);
    n = ler_cartoes(html, cartoes, CX_POR_FIL);
    free(html); html = NULL;
    if (n <= 0 || nf >= CX_FIL_MAX || ni + n > CX_ITENS_MAX) continue;
    for (j = 0; j < n; j++) item_de(&cartoes[j], &itens[ni + j]);
    decodificar_nome(cats[k], strlen(cats[k]), nome, sizeof nome);
    snprintf(fils[nf].chave, sizeof fils[nf].chave, "cx_series_%d", k);
    snprintf(fils[nf].titulo, sizeof fils[nf].titulo, "Cineplex · %s", nome);
    copiar(fils[nf].tipo, sizeof fils[nf].tipo, "series");
    copiar(fils[nf].base, sizeof fils[nf].base, "cineplex:");
    snprintf(fils[nf].catId, sizeof fils[nf].catId, "c:%s", cats[k]);
    fils[nf].ini = ni; fils[nf].n = n;
    ni += n; nf++;
  }
  pthread_mutex_lock(&trava);
  if (g == geracao) {
    if (!snapItens) snapItens = malloc(sizeof(CatItem) * CX_ITENS_MAX);
    if (snapItens && nf > 0) {
      memcpy(snapItens, itens, sizeof(CatItem) * (size_t)ni);
      memcpy(snapFils, fils, sizeof(CatFileira) * (size_t)nf);
      nSnapFils = nf;
      nCatsReg = nCats;
      for (k = 0; k < nCats; k++) { copiar(catEnc[k], sizeof catEnc[k], cats[k]); catPag[k] = pags[k]; }
      snapVersao++;
      carregado = 1;
      snapGeracao = g;
      printf("[cineplex] library: %d row(s), %d title(s), %d categories\n", nf, ni, nCats);
      fflush(stdout);
    } else if (nf == 0) {
      printf("[cineplex] library: no rows (server unreachable?)\n");
      fflush(stdout);
    }
  }
  pthread_mutex_unlock(&trava);
fim:
  pthread_mutex_lock(&trava);
  carregando = 0;
  pthread_mutex_unlock(&trava);
  free(html); free(itens); free(cartoes); free(cats); free(pags);
  return NULL;
}
static void iniciarCarga(void) {
  pthread_t t;
  pthread_mutex_lock(&trava);
  if (carregando || carregado) { pthread_mutex_unlock(&trava); return; }
  carregando = 1;
  pthread_mutex_unlock(&trava);
  if (pthread_create(&t, NULL, carregarFio, (void *)(uintptr_t)geracao) == 0) pthread_detach(t);
  else { pthread_mutex_lock(&trava); carregando = 0; pthread_mutex_unlock(&trava); }
}

void cineplex_carregar(void) { baseIniciar(); iniciarCarga(); }
void cineplex_perfil_trocou(void) {
  pthread_mutex_lock(&trava);
  geracao++;
  carregado = 0; carregando = 0;
  pthread_mutex_unlock(&trava);
  iniciarCarga();
}
void cineplex_esquecer_todos(void) {
  pthread_mutex_lock(&trava);
  memset(jar, 0, sizeof jar);
  nJar = 0;
  pthread_mutex_unlock(&trava);
}
int cineplex_ligado(void) { return 1; }
int cineplex_conectado(void) {
  int ok;
  pthread_mutex_lock(&trava);
  ok = carregado && nSnapFils > 0;
  pthread_mutex_unlock(&trava);
  return ok;
}
unsigned cineplex_fileiras_versao(void) {
  unsigned v;
  pthread_mutex_lock(&trava);
  v = snapVersao;
  pthread_mutex_unlock(&trava);
  return v;
}
int cineplex_chave_fileira(const char *chave) { return chave && !strncmp(chave, "cx_series_", 10); }
int cineplex_fileiras_copiar(CatItem *itens, int maxItens, CatFileira *fils, int maxFils, int *nItens) {
  int r, nf = 0, ni = 0;
  if (nItens) *nItens = 0;
  pthread_mutex_lock(&trava);
  for (r = 0; carregado && r < nSnapFils && nf < maxFils; r++) {
    const CatFileira *f = &snapFils[r];
    if (ni + f->n > maxItens) break;
    memcpy(itens + ni, snapItens + f->ini, sizeof(CatItem) * (size_t)f->n);
    fils[nf] = *f;
    fils[nf].ini = ni;
    ni += f->n;
    nf++;
  }
  pthread_mutex_unlock(&trava);
  if (nItens) *nItens = ni;
  return nf;
}

// ---------------------------------------------------------- watch page
typedef struct {
  char titulo[160], ano[8], sinopse[900], poster[300];
  int temps[CX_TEMPS_MAX], nTemps, selecionada;
} Pagina;

typedef struct { int ep; char thumb[300]; } EpLido;

static void caminho_watch(char *dst, size_t tam, unsigned long id, int temp, int ep, int auto_) {
  snprintf(dst, tam, "/watch.php?id=%lu&series_id=%lu&season=%d&ep=%d%s", id, id, temp, ep, auto_ ? "&autoplay=1" : "");
}

static int ler_episodios(const char *html, EpLido *out, int max) {
  const char *fim = html + strlen(html), *p = html;
  int n = 0, i;
  while (n < max && (p = buscar(p, fim, "data-ep=\"")) != NULL) {
    int ep = atoi(p + 9);
    const char *a1 = buscar(p, fim, "</a>");
    char tmp[320];
    if (!a1) a1 = fim;
    for (i = 0; i < n; i++) if (out[i].ep == ep) break;
    if (ep > 0 && i == n) {
      out[n].ep = ep;
      out[n].thumb[0] = 0;
      tmp[0] = 0;
      if (!atributo(p, a1, "data-src=\"", tmp, sizeof tmp) || !tmp[0]) atributo(p, a1, " src=\"", tmp, sizeof tmp);
      absoluta(tmp, out[n].thumb, sizeof out[n].thumb);
      n++;
    }
    p = a1;
  }
  return n;
}

static void ler_cabecalho(const char *html, Pagina *pg) {
  const char *fim = html + strlen(html), *h = buscar(html, fim, "<h1"), *q;
  pg->titulo[0] = pg->ano[0] = pg->sinopse[0] = pg->poster[0] = 0;
  if (h) {
    const char *fh = buscar(h, fim, "</h1>"), *sp;
    h = memchr(h, '>', (size_t)(fim - h));
    if (h && fh) {
      sp = buscar(h, fh, "<span");
      texto(h + 1, sp ? sp : fh, pg->titulo, sizeof pg->titulo);
      if (sp) {
        char t[32];
        texto(sp, fh, t, sizeof t);
        for (q = t; *q && !isdigit((unsigned char)*q); q++) {}
        if (isdigit((unsigned char)q[0]) && isdigit((unsigned char)q[1]) && isdigit((unsigned char)q[2]) && isdigit((unsigned char)q[3]))
          snprintf(pg->ano, sizeof pg->ano, "%.4s", q);
      }
    }
  }
  if ((q = buscar(html, fim, "id=\"synopsis\"")) && (q = memchr(q, '>', (size_t)(fim - q)))) {
    const char *e = buscar(q, fim, "</p>");
    const char *e2 = buscar(q, fim, "</div>");
    if (!e || (e2 && e2 < e)) e = e2;
    texto(q + 1, e ? e : fim, pg->sinopse, sizeof pg->sinopse);
  }
  if ((q = buscar(html, fim, "<video"))) {
    char tmp[320];
    const char *fv = memchr(q, '>', (size_t)(fim - q));
    if (fv && atributo(q, fv, "poster=\"", tmp, sizeof tmp)) absoluta(tmp, pg->poster, sizeof pg->poster);
  }
}

static void ler_temporadas(const char *html, Pagina *pg) {
  const char *fim = html + strlen(html), *s = buscar(html, fim, "name=\"season\""), *e, *p;
  pg->nTemps = 0; pg->selecionada = 0;
  if (!s) return;
  e = buscar(s, fim, "</select>");
  if (!e) e = fim;
  p = s;
  while (pg->nTemps < CX_TEMPS_MAX && (p = buscar(p, e, "<option")) != NULL) {
    const char *f = memchr(p, '>', (size_t)(e - p));
    char v[16];
    if (!f) break;
    if (atributo(p, f, "value=\"", v, sizeof v) && isdigit((unsigned char)v[0])) {
      int t = atoi(v), i;
      for (i = 0; i < pg->nTemps; i++) if (pg->temps[i] == t) break;
      if (i == pg->nTemps) pg->temps[pg->nTemps++] = t;
      if (buscar(p, f, "selected")) pg->selecionada = t;
    }
    p = f;
  }
}

int cineplex_ficha(CatItem *item, CatEp *eps, int maxEps) {
  unsigned long serie;
  int temp = 0, ep = 0, ne = 0, i, k, primeira;
  char caminho[200], *html = NULL;
  Pagina *pg;
  EpLido *lidos;
  if (!item || !cineplex_id_partes(item->imdb, &serie, &temp, &ep) || temp != 0) return -1;
  pg = malloc(sizeof *pg);
  lidos = malloc(sizeof(EpLido) * CX_EPS_PAGINA);
  if (!pg || !lidos) { free(pg); free(lidos); return -1; }
  memset(pg, 0, sizeof *pg);
  caminho_watch(caminho, sizeof caminho, serie, 1, 1, 0);
  if (!pegar(caminho, &html, NULL)) { free(pg); free(lidos); return -1; }
  ler_cabecalho(html, pg);
  ler_temporadas(html, pg);
  if (pg->nTemps == 0) { pg->temps[0] = 1; pg->nTemps = 1; pg->selecionada = 1; }
  if (!pg->selecionada) pg->selecionada = 1;
  if (pg->titulo[0]) copiar(item->titulo, sizeof item->titulo, pg->titulo);
  if (pg->sinopse[0]) copiar(item->sinopse, sizeof item->sinopse, pg->sinopse);
  if (pg->ano[0]) copiar(item->meta, sizeof item->meta, pg->ano);
  if (pg->poster[0] && !item->poster[0]) copiar(item->poster, sizeof item->poster, pg->poster);
  item->nTemporadas = 0;
  primeira = 1;
  for (i = 0; i < pg->nTemps && ne < maxEps; i++) {
    int t = pg->temps[i], n;
    if (!(primeira && t == pg->selecionada)) {
      char *h2 = NULL;
      caminho_watch(caminho, sizeof caminho, serie, t, 1, 0);
      if (!pegar(caminho, &h2, NULL)) continue;
      n = ler_episodios(h2, lidos, CX_EPS_PAGINA);
      free(h2);
    } else {
      n = ler_episodios(html, lidos, CX_EPS_PAGINA);
    }
    if (t == pg->selecionada) primeira = 0;
    if (n <= 0) continue;
    if (item->nTemporadas < CAT_TEMP_MAX) item->temporadas[item->nTemporadas++] = t;
    for (k = 0; k < n && ne < maxEps; k++) {
      CatEp *e = &eps[ne];
      memset(e, 0, sizeof *e);
      e->temporada = t;
      e->episodio = lidos[k].ep;
      snprintf(e->nome, sizeof e->nome, "Episode %d", lidos[k].ep);
      copiar(e->thumb, sizeof e->thumb, lidos[k].thumb);
      cineplex_id_episodio(e->vid, sizeof e->vid, serie, t, lidos[k].ep);
      ne++;
    }
  }
  free(html); free(pg); free(lidos);
  return ne;
}

// -------------------------------------------------------------- sources
static struct {
  char alvo[JFID_MAX + 8];
  int estado;
  Stream *lista;
  int n;
  unsigned ger;
} fontes;

typedef struct { char alvo[JFID_MAX + 8]; unsigned ger; } TarefaFonte;

static void *fontesFio(void *u) {
  TarefaFonte *t = u;
  unsigned long serie;
  int temp = 1, ep = 1, ok = 0;
  char caminho[200], *html = NULL, url[420] = "", cab[512];
  Stream *s = NULL;
  if (cineplex_id_partes(t->alvo, &serie, &temp, &ep)) {
    if (temp == 0 && ep == 0) { temp = 1; ep = 1; }
    caminho_watch(caminho, sizeof caminho, serie, temp, ep, 1);
    if (pegar(caminho, &html, NULL)) {
      const char *fim = html + strlen(html), *q;
      char tmp[400] = "";
      if ((q = buscar(html, fim, "<source"))) {
        const char *f = memchr(q, '>', (size_t)(fim - q));
        if (f) atributo(q, f, "src=\"", tmp, sizeof tmp);
      }
      if (!tmp[0] && (q = buscar(html, fim, "master.m3u8"))) {
        const char *b = q;
        while (b > html && b[-1] != '"' && b[-1] != '\'' && !isspace((unsigned char)b[-1]) && b[-1] != '>') b--;
        if ((size_t)(q + 11 - b) < sizeof tmp) { memcpy(tmp, b, (size_t)(q + 11 - b)); tmp[q + 11 - b] = 0; }
      }
      if (tmp[0]) { absoluta(tmp, url, sizeof url); ok = url[0] != 0; }
      free(html);
    }
  }
  if (ok) {
    char ck[256];
    int w;
    s = calloc(1, sizeof *s);
    if (!s) ok = 0;
    else {
      snprintf(s->rotulo, sizeof s->rotulo, "Cineplex · S%02d E%02d", temp, ep);
      copiar(s->provedor, sizeof s->provedor, "Cineplex");
      copiar(s->url, sizeof s->url, url);
      s->fileIdx = -1;
      cookieCabecalho(ck, sizeof ck);
      w = snprintf(cab, sizeof cab, "User-Agent: %s\nReferer: %s/%s%s", CX_UA, base, ck[0] ? "\nCookie: " : "", ck);
      if (w < 0 || (size_t)w >= sizeof s->cabecalhos)   // too long: keep the cookie, drop the Referer
        w = snprintf(cab, sizeof cab, "User-Agent: %s%s%s", CX_UA, ck[0] ? "\nCookie: " : "", ck);
      if (w < 0 || (size_t)w >= sizeof s->cabecalhos)
        w = snprintf(cab, sizeof cab, "%s%s", ck[0] ? "Cookie: " : "", ck);
      copiar(s->cabecalhos, sizeof s->cabecalhos, cab);
      printf("[cineplex] stream for S%02dE%02d ready (%s)\n", temp, ep, ck[0] ? "with cookies" : "NO cookies");
      fflush(stdout);
    }
  }
  pthread_mutex_lock(&trava);
  if (t->ger == fontes.ger && !strcmp(t->alvo, fontes.alvo)) {
    free(fontes.lista);
    fontes.lista = ok ? s : NULL;
    fontes.n = ok ? 1 : 0;
    fontes.estado = ok ? CX_FONTES_PRONTO : CX_FONTES_FALHOU;
    s = NULL;
  }
  pthread_mutex_unlock(&trava);
  free(s);
  free(t);
  return NULL;
}

int cineplex_fontes_pedir(const char *alvo) {
  TarefaFonte *t;
  pthread_t th;
  if (!alvo || !cineplex_id_partes(alvo, NULL, NULL, NULL)) return 0;
  t = calloc(1, sizeof *t);
  if (!t) return 0;
  pthread_mutex_lock(&trava);
  free(fontes.lista); fontes.lista = NULL; fontes.n = 0;
  copiar(fontes.alvo, sizeof fontes.alvo, alvo);
  fontes.estado = CX_FONTES_PENDENTE;
  fontes.ger++;
  copiar(t->alvo, sizeof t->alvo, alvo);
  t->ger = fontes.ger;
  pthread_mutex_unlock(&trava);
  if (pthread_create(&th, NULL, fontesFio, t) != 0) {
    pthread_mutex_lock(&trava); fontes.estado = CX_FONTES_FALHOU; pthread_mutex_unlock(&trava);
    free(t);
    return 0;
  }
  pthread_detach(th);
  return 1;
}

int cineplex_fontes_colher(const char *alvo, Stream **lista, int *n) {
  int e;
  if (lista) *lista = NULL;
  if (n) *n = 0;
  pthread_mutex_lock(&trava);
  if (!alvo || strcmp(alvo, fontes.alvo)) { pthread_mutex_unlock(&trava); return CX_FONTES_FALHOU; }
  e = fontes.estado;
  if (e == CX_FONTES_PRONTO) {
    if (lista) { *lista = fontes.lista; fontes.lista = NULL; }
    if (n) *n = fontes.n;
    fontes.n = 0;
    fontes.estado = CX_FONTES_NADA;
  } else if (e == CX_FONTES_FALHOU) fontes.estado = CX_FONTES_NADA;
  pthread_mutex_unlock(&trava);
  return e;
}


// ------------------------------------------------------------ see all
int cineplex_pagina(const char *catId, int pagina, CatItem *saida, int max, int *fim) {
  char caminho[220], *html = NULL;
  char enc[96] = "";
  int pg = pagina < 1 ? 1 : pagina, total = 0, n = 0, i, ultima = 0;
  Cartao *c;
  if (fim) *fim = 0;
  if (!catId || !saida || max <= 0) return -1;
  if (!strncmp(catId, "c:", 2)) {
    copiar(enc, sizeof enc, catId + 2);
    pthread_mutex_lock(&trava);
    for (i = 0; i < nCatsReg; i++) if (!strcmp(catEnc[i], enc)) { total = catPag[i]; break; }
    pthread_mutex_unlock(&trava);
  } else if (!strcmp(catId, "all")) {
    int g = pg;
    pthread_mutex_lock(&trava);
    for (i = 0; i < nCatsReg; i++) {
      int pk = catPag[i] > 0 ? catPag[i] : 1;
      if (g <= pk) { copiar(enc, sizeof enc, catEnc[i]); total = pk; pg = g; ultima = (i == nCatsReg - 1); break; }
      g -= pk;
    }
    pthread_mutex_unlock(&trava);
    if (!enc[0]) { *fim = 1; return 0; }      // walked past the last category
  } else return -1;
  snprintf(caminho, sizeof caminho, "/tcategory.php?category=%s&page=%d", enc, pg);
  if (!pegar(caminho, &html, NULL)) return -1;
  c = malloc(sizeof(Cartao) * 96);
  if (!c) { free(html); return -1; }
  if (total <= 0) total = total_paginas(html);
  n = ler_cartoes(html, c, max < 96 ? max : 96);
  for (i = 0; i < n; i++) { item_de(&c[i], &saida[i]); copiar(saida[i].origem, sizeof saida[i].origem, "Cineplex"); }
  if (fim) {
    if (!strcmp(catId, "all")) *fim = ultima && pg >= total;
    else *fim = (total > 0 && pg >= total) || n == 0;
  }
  free(c); free(html);
  return n;
}

// ------------------------------------------------------------- search
// Minimal JSON reading for the flat objects search_ajax.php returns.
static const char *json_fim_valor(const char *p, const char *fim) {   // p at '{' or '['
  int prof = 0, str = 0;
  for (; p < fim; p++) {
    if (str) { if (*p == '\\') p++; else if (*p == '"') str = 0; continue; }
    if (*p == '"') str = 1;
    else if (*p == '{' || *p == '[') prof++;
    else if (*p == '}' || *p == ']') { if (--prof == 0) return p + 1; }
  }
  return NULL;
}
static void utf8(unsigned cp, char *dst, size_t *k, size_t tam) {
  if (cp < 0x80 && *k + 1 < tam) dst[(*k)++] = (char)cp;
  else if (cp < 0x800 && *k + 2 < tam) { dst[(*k)++] = (char)(0xC0 | (cp >> 6)); dst[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000 && *k + 3 < tam) { dst[(*k)++] = (char)(0xE0 | (cp >> 12)); dst[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
  else if (*k + 4 < tam) { dst[(*k)++] = (char)(0xF0 | (cp >> 18)); dst[(*k)++] = (char)(0x80 | ((cp >> 12) & 0x3F)); dst[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
}
// value of "chave" in the flat object [o, fim): strings (escapes decoded) or bare numbers.
static int json_valor(const char *o, const char *fim, const char *chave, char *dst, size_t tam) {
  char ag[48];
  const char *p;
  size_t k = 0;
  if (tam) dst[0] = 0;
  snprintf(ag, sizeof ag, "\"%s\"", chave);
  p = buscar(o, fim, ag);
  if (!p) return 0;
  p += strlen(ag);
  while (p < fim && (isspace((unsigned char)*p) || *p == ':')) p++;
  if (p >= fim) return 0;
  if (*p != '"') {
    while (p < fim && *p != ',' && *p != '}' && !isspace((unsigned char)*p) && k + 1 < tam) dst[k++] = *p++;
    dst[k] = 0;
    return strcmp(dst, "null") != 0 && k > 0;
  }
  for (p++; p < fim && *p != '"' && k + 1 < tam; p++) {
    if (*p != '\\') { dst[k++] = *p; continue; }
    if (++p >= fim) break;
    switch (*p) {
      case 'n': case 't': case 'r': dst[k++] = ' '; break;
      case 'u': {
        unsigned cp = 0; int d;
        if (p + 4 >= fim) { dst[k] = 0; return k > 0; }
        for (d = 1; d <= 4; d++) cp = cp * 16 + (unsigned)(isdigit((unsigned char)p[d]) ? p[d] - '0' : (tolower((unsigned char)p[d]) - 'a' + 10));
        p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < fim && p[1] == '\\' && p[2] == 'u') {   // surrogate pair
          unsigned lo = 0;
          for (d = 3; d <= 6; d++) lo = lo * 16 + (unsigned)(isdigit((unsigned char)p[d]) ? p[d] - '0' : (tolower((unsigned char)p[d]) - 'a' + 10));
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          p += 6;
        }
        utf8(cp, dst, &k, tam);
        break;
      }
      default: dst[k++] = *p;   // \" \\ \/
    }
  }
  dst[k] = 0;
  return k > 0;
}

int cineplex_buscar(const char *termo, CatItem *saida, int max) {
  char caminho[400], esc[300], *json = NULL;
  const char *p, *fim, *arr;
  size_t k = 0;
  int n = 0, i;
  if (!termo || !*termo || !saida || max <= 0) return 0;
  for (const unsigned char *c = (const unsigned char *)termo; *c && k + 4 < sizeof esc; c++) {
    if (isalnum(*c) || *c == '-' || *c == '_' || *c == '.') esc[k++] = (char)*c;
    else if (*c == ' ') esc[k++] = '+';
    else k += (size_t)snprintf(esc + k, 4, "%%%02X", *c);
  }
  esc[k] = 0;
  snprintf(caminho, sizeof caminho, "/search_ajax.php?q=%s&limit=30", esc);
  if (!pegar(caminho, &json, NULL)) return 0;
  fim = json + strlen(json);
  arr = buscar(json, fim, "\"results\"");
  arr = arr ? memchr(arr, '[', (size_t)(fim - arr)) : NULL;
  for (p = arr ? arr + 1 : NULL; p && p < fim && n < max;) {
    const char *o = memchr(p, '{', (size_t)(fim - p)), *f;
    Cartao c;
    char id[24], url[200], tipo[40], poster[300], ano[16];
    if (!o) break;
    f = json_fim_valor(o, fim);
    if (!f) break;
    memset(&c, 0, sizeof c);
    url[0] = tipo[0] = 0;
    json_valor(o, f, "url", url, sizeof url);
    json_valor(o, f, "type", tipo, sizeof tipo);
    if ((strstr(url, "tview.php") || buscar(tipo, tipo + strlen(tipo), "eries") != NULL) &&
        json_valor(o, f, "id", id, sizeof id) && json_valor(o, f, "title", c.titulo, sizeof c.titulo)) {
      c.id = strtoul(id, NULL, 10);
      poster[0] = ano[0] = 0;
      json_valor(o, f, "poster", poster, sizeof poster);
      json_valor(o, f, "year", ano, sizeof ano);
      json_valor(o, f, "category", c.cat, sizeof c.cat);
      absoluta(poster, c.poster, sizeof c.poster);
      if (strlen(ano) >= 4 && isdigit((unsigned char)ano[0])) snprintf(c.ano, sizeof c.ano, "%.4s", ano);
      if (c.id) {
        for (i = 0; i < n; i++) { unsigned long x; if (cineplex_id_partes(saida[i].imdb, &x, NULL, NULL) && x == c.id) break; }
        if (i == n) { item_de(&c, &saida[n]); copiar(saida[n].origem, sizeof saida[n].origem, "Cineplex"); n++; }
      }
    }
    p = f;
  }
  free(json);
  return n;
}

void cineplex_reproducao_tick(const char *url, double posSeg, double durSeg, int tocando) { (void)url; (void)posSeg; (void)durSeg; (void)tocando; }
void cineplex_reproducao_fim(const char *url, double posSeg, double durSeg) { (void)url; (void)posSeg; (void)durSeg; }
