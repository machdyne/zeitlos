#ifndef AIDX_H
#define AIDX_H

/*
 * ask -- the index reader and query engine.
 *
 * This file and aidx.c contain NO window code, NO messaging and no
 * device-specific I/O. Everything reaches the card through the six
 * functions in the shim below, which are fs_* on the device and stdio
 * on the host. That is what lets test/hosttest.c run the REAL engine
 * against a REAL pack built by tools/ask -- the same arrangement
 * sw/apps/chess uses for ce_*.c, and for the same reason: a retrieval
 * bug found on a 48MHz board through a 320x300 window is a bug found
 * the expensive way.
 *
 * -- what is resident and what is not --
 *
 * Only the COARSE VECTOR ARRAY and the query ENCODER are held in
 * memory. Everything else -- the chunk table, the document table and
 * its strings, the term dictionary, the postings, the fine vectors --
 * is read off the card when needed.
 *
 * That is not a memory-saving afterthought, it is the shape of the
 * workload. The coarse array is touched in its entirety on every
 * query, so it has to be resident or every query re-reads it. The
 * chunk and document tables are touched about eight times per query,
 * once per displayed hit. The term dictionary is binary-searched,
 * which is ~16 seeks. The fine vectors are read for the shortlist
 * only. None of those justify holding megabytes.
 *
 * For the arkmed pack that is 395KB + 566KB resident against 2.5MB if
 * everything were loaded, and about 2.4 seconds of card read at
 * startup instead of six.
 *
 * -- integer arithmetic only --
 *
 * No floats anywhere. Vectors are int8 and a dot product is an int32;
 * BM25 is evaluated in Q16 fixed point. picorv32 has no FPU
 * (rtl/boards.vh) so a float would be a libgcc call per operation, and
 * ranking only needs an ORDER, which integers give exactly.
 */

#include <stdint.h>
#include <stdbool.h>

/* -- on-card format constants, mirroring tools/ask/lib/pack.py --
 *
 * If these disagree with the packer, the header check fails and the
 * pack is refused. That is the intended outcome: a silently
 * misinterpreted index produces confidently ranked results pointing at
 * the wrong paragraphs, which is the worst thing this program can do.
 */
#define AI_FORMAT_VERSION   1

#define AI_MAGIC_INDEX      0x314B415AUL    /* "ZAK1" */
#define AI_MAGIC_DOCS       0x3154445AUL    /* "ZDT1" */
#define AI_MAGIC_CHUNKS     0x3154435AUL    /* "ZCT1" */
#define AI_MAGIC_COARSE     0x3156435AUL    /* "ZCV1" */
#define AI_MAGIC_FINE       0x3156465AUL    /* "ZFV1" */
#define AI_MAGIC_LEXICON    0x31584C5AUL    /* "ZLX1" */
#define AI_MAGIC_POST       0x31504C5AUL    /* "ZLP1" */
#define AI_MAGIC_ENCODER    0x31444D5AUL    /* "ZMD1" */

#define AI_HDR_SIZE         32

#define AI_FLAG_HAS_FINE    (1u << 0)
#define AI_FLAG_HAS_LEXICON (1u << 1)
#define AI_FLAG_HAS_ENCODER (1u << 2)

/* -- limits -- */

#define AI_PACKS_MAX        4       /* installed packs queried at once */
/* Entries read from /ask in one listing. Larger than AI_PACKS_MAX
 * because the directory may hold files as well as pack directories,
 * and the type array must line up with the names one-for-one. */
#define AI_LIST_MAX         32
#define AI_HITS_MAX         16
#define AI_SHORTLIST        512     /* measured, not chosen: see below */
#define AI_QUERY_MAX        160
#define AI_PATH_MAX         96      /* Z_WM_ARG_MAX, zwm.h */
#define AI_TITLE_MAX        64
#define AI_HEAD_MAX         64
#define AI_TERMS_MAX        12      /* query terms actually looked up */

/* Candidates kept from each half before they are fused. Both lists are
 * cut to this, then merged by reciprocal rank -- so the fusion sees a
 * comparable depth from each side rather than whichever half happened
 * to be more confident. */
#define AI_FUSE_DEPTH       64

/* How much the dense list counts for in the fusion, in Q8. 256 is
 * plain reciprocal rank fusion (equal weight), which is what
 * tools/ask uses by default.
 *
 * It is a knob because equal weight demonstrably wastes the dense
 * half: on a 112-question set the trained encoder gained +7 at rank 1
 * ON ITS OWN while the fused result moved by one. RRF lets the
 * stronger list dominate, and with BM25 at 91% on keyword queries the
 * stronger list is nearly always lexical.
 *
 * MUST MATCH `fuse_dense` in the recipe, or the device ranks
 * differently from everything tools/ask measured. Find it with
 * `ask eval <recipe> --fuse-sweep 0.25,0.5,1,2`. */
#define AI_FUSE_DENSE_Q8    256

/* Postings accumulator. Open addressing, power of two, and bounded.
 *
 * -- ADMIT, THEN CONTINUE --
 *
 * A query's terms are processed RAREST FIRST (lowest df, highest idf),
 * and a chunk is admitted to the accumulator only while the pack's
 * share of AI_LEXMAP_ADMIT is not used up. After that, later terms --
 * the common ones -- only ADD to chunks already present. This is the
 * standard way to run term-at-a-time BM25 in bounded memory (Moffat
 * and Zobel's "continue" strategy): a chunk that matches none of the
 * informative terms cannot outrank one that does on the strength of a
 * common word alone, so what is refused admission is what would have
 * ranked low anyway.
 *
 * It replaced a map of 1024 slots that took chunks in corpus order
 * and dropped whatever arrived once it was full. On `arklite` that
 * overflowed for 54 of the 127 gold questions and changed the rank-1
 * passage on 10 of them, measured against exact BM25 over the same
 * packed files (sw/apps/ask/test/lexcheck.py). Which chunks survived
 * depended on where they sat in the corpus, not on relevance -- and
 * at `arkmed` scale everything late in the corpus (Wikipedia,
 * MedlinePlus) would have been the part dropped.
 *
 * ADMIT stays below SIZE so a probe always finds an empty slot and
 * the map can never fill. 4096 slots is 48KB of the query state. */
#define AI_LEXMAP_BITS      12
#define AI_LEXMAP_SIZE      (1 << AI_LEXMAP_BITS)
#define AI_LEXMAP_ADMIT     (AI_LEXMAP_SIZE * 3 / 4)

/* Postings are streamed in blocks of this size, one block per
 * ai_query_step(). The first version read ONE block and ignored the
 * rest of the list: harmless on `arklite`, whose longest list fits, and
 * at `arkmed` scale it would have silently scored only the chunks at
 * the start of the corpus for every common term. */
#define AI_POST_BLOCK       4096

/* BM25 k1 and b, in Q8. */
#define AI_BM25_K1_Q8       307     /* 1.2  */
#define AI_BM25_B_Q8        192     /* 0.75 */

/* Bytes per token, for turning a chunk's byte length into a token
 * count. Measured across the Ark corpora at a little over six. Only
 * the RATIO to the average matters, so the constant cancels -- it is
 * named rather than inlined because it is an estimate, not a fact. */
#define AI_BYTES_PER_TOKEN  6

/*
 * AI_SHORTLIST is a measured floor, not a tuning knob.
 *
 * `ask eval` on arklite+zdocs, 63 gold questions, reports how deep the
 * coarse shortlist has to be before the fine stage can still see the
 * right chunk:
 *
 *     top 128   54/63
 *     top 256   60/63
 *     top 512   62/63
 *     top 1024  63/63
 *
 * Below 512 the answer is discarded before anything good looks at it,
 * and no fine-stage accuracy recovers it. 512 x 128 bytes is 64KB of
 * fine vectors read per query, about 110-220ms at a realistic
 * 300-600 KB/s.
 */

/* Vectors scanned between yields. See zask.h's Z_ASK_SCAN_SLICE for
 * why this number is the responsiveness dial. */
#define AI_SCAN_SLICE       2048

/* Shortlist entries re-ranked between yields.
 *
 * Much smaller than AI_SCAN_SLICE because the work per item is not
 * comparable: a coarse-scan item is a 32-element dot product against
 * resident memory, a re-rank item is a seek and a read from the card.
 * 64 of those is tens of milliseconds; 512 -- the whole shortlist,
 * which is what this did in ONE step -- is seconds of a frozen window
 * right after the progress bar reaches 100%. */
#define AI_RERANK_SLICE     64

/* -- the file shim --
 *
 * fs_* on the device, stdio on the host. Six functions, all of which
 * the device already has.
 *
 * *** THE RETURN CONVENTION IS THIS FILE'S, NOT EITHER BACKEND'S. ***
 *
 *   ai_open   0 on success, < 0 on failure
 *   ai_seek   0 on success, < 0 on failure
 *   ai_read   bytes read, < 0 on failure
 *   ai_size   size in bytes, <= 0 if absent
 *
 * Both backends must be translated INTO that, not passed through.
 * This is written out because getting it wrong cost a debugging
 * session on real hardware: fs_seek() returns 1 for success and 0 for
 * failure, fseek() returns 0 for success, and the shim returned each
 * verbatim. Every seek therefore "failed" on the device and succeeded
 * on the host -- so test/hosttest.c passed, the pack loaded (loading
 * never seeks), the scan ran to 100%, and every single query produced
 * zero hits, because fill_hit() and fine_score() do nothing but seek.
 *
 * The shim is the one part of this file the host test cannot check.
 * ai_selftest() below exists for exactly that reason.
 */
typedef struct {
    int   handle;               /* < 0 when closed */
    void *fp;                   /* host only; unused on the device */
} ai_file_t;

int  ai_open(ai_file_t *f, const char *path);
int  ai_read(ai_file_t *f, void *buf, int len);
int  ai_seek(ai_file_t *f, uint32_t off);
void ai_close(ai_file_t *f);
int  ai_size(const char *path);
/* Lists the names of subdirectories under `path`. Returns a count and
 * fills `names` with NUL-terminated entries, each at most 13 bytes
 * (8.3 plus NUL). */
int  ai_listdirs(const char *path, char *names, int maxnames);

/* Proves the shim works on whatever this is running on: opens a file,
 * seeks into it, and checks the bytes are the ones a sequential read
 * gave. Returns 0 on success.
 *
 * Called once per pack load. A shim whose seek is inverted cannot be
 * caught by the host test and otherwise shows up as "every query
 * returns nothing", which reads like a retrieval problem and is not
 * one. */
int  ai_selftest(const char *path);

/* -- a loaded pack -- */
typedef struct {
    char        name[16];       /* "arklite", from the directory name */
    char        dir[32];        /* "/ask/arklite" */
    char        cardroot[32];   /* "/ark/arklite" */

    uint32_t    dsid;
    uint32_t    flags;
    uint32_t    ndocs;
    uint32_t    nchunks;
    uint32_t    coarse_dim;
    uint32_t    fine_dim;
    uint32_t    nterms;
    uint32_t    vocab;
    uint32_t    encoder_id;
    uint32_t    avg_len_q8;     /* BM25 length norm, Q24.8 */

    int8_t     *coarse;         /* nchunks * coarse_dim, resident */

    /* Token count per chunk, for BM25's length normalisation.
     *
     * Built at load time from chunks.zct's byte lengths, which are
     * already there for the preview -- so this needs no new file and
     * no format change. Two bytes a chunk: 20KB for arklite. */
    uint16_t   *lens;
    uint32_t    avg_len;        /* mean of lens[], in tokens */

    /* encoder, resident: vocab*dim int8 vectors, then u16 idf, then
     * u32 term offsets, then the term string pool. */
    int8_t     *enc_vec;
    uint16_t   *enc_idf;
    uint32_t   *enc_off;
    char       *enc_pool;
    uint32_t    enc_pool_len;

    uint32_t    resident;       /* bytes this pack is holding */
    bool        ok;
} ai_pack_t;

/* -- one hit -- */
typedef struct {
    const ai_pack_t *pack;
    uint32_t    chunk;
    int32_t     score;          /* 0..1000, see zask.h on what it means */
    uint32_t    off;
    uint32_t    len;
    char        path[AI_PATH_MAX];
    char        title[AI_TITLE_MAX];
    char        head[AI_HEAD_MAX];
} ai_hit_t;

/* One shortlist entry. Named rather than anonymous so the heap code
 * can swap two of them without a GCC typeof(). */
typedef struct {
    int32_t  score;
    uint32_t chunk;
    int8_t   pack;
} ai_cand_t;

/* One query term's entry in a pack's lexical plan. Named, like
 * ai_cand_t, so the sort can swap two without a GCC typeof(). */
typedef struct {
    uint32_t off;           /* postings offset in post.zlp */
    uint32_t len;           /* postings bytes */
    uint32_t df;
    int32_t  idf;           /* Q8 */
} ai_lplan_t;

/* -- query state --
 *
 * A query is a state machine so the caller can return to its message
 * loop between slices. ai_query_step() does a bounded amount of work
 * and returns false while there is more to do.
 */
typedef enum {
    AI_Q_IDLE = 0,
    AI_Q_ENCODE,
    AI_Q_SCAN,
    AI_Q_LEXICAL,
    AI_Q_RERANK,
    AI_Q_SELECT,
    AI_Q_DONE,
    AI_Q_CANCELLED,
} ai_phase_t;

typedef struct {
    ai_phase_t  phase;
    int         pack_i;         /* pack being scanned */
    uint32_t    scan_i;         /* vector within that pack */
    uint32_t    scanned;        /* total, for progress */
    uint32_t    total;          /* expected total, for progress */

    char        text[AI_QUERY_MAX];   /* the query, as typed */

    /* The query encoded into EVERY pack's space, once, up front.
     * Packs may carry different encoders and a vector from one is
     * meaningless in another's, so this used to be re-encoded per
     * pack during the scan and then again PER SHORTLIST ENTRY during
     * the re-rank -- 512 encodes for one query. */
    int8_t      pvec[AI_PACKS_MAX][256];
    uint32_t    pdim[AI_PACKS_MAX];

    uint32_t    rr_i;           /* re-rank cursor into the shortlist */
    ai_file_t   fz;             /* fine.zfv, held open across slices */
    int         fz_pack;        /* which pack fz belongs to, -1 if none */
    uint32_t    fz_n;           /* its vector count and dimension, read */
    uint32_t    fz_dim;         /* from the header once per pack */

    /* Lexical half: BM25 accumulator, then the top AI_FUSE_DEPTH. */
    struct {
        uint32_t chunk;
        int32_t  score;
        int8_t   pack;
        uint8_t  used;
    } lexmap[AI_LEXMAP_SIZE];
    ai_cand_t   lex[AI_FUSE_DEPTH];
    int         nlex;
    int         lex_pack;       /* pack whose terms are being walked */
    int         lex_term;       /* unused since the plan; kept for ABI */

    /* The pack's query plan: every query term found in its dictionary,
     * rarest first. Built once per pack, then walked block by block. */
    ai_lplan_t  lplan[AI_TERMS_MAX];
    int         nplan;          /* -1 until built for lex_pack */
    int         plan_i;         /* term being streamed */
    uint32_t    plan_done;      /* bytes of it consumed */
    uint32_t    plan_cid;       /* running chunk id (delta decoding) */
    uint32_t    lex_admitted;   /* chunks admitted for lex_pack */
    uint32_t    lex_budget;     /* ...and its share of the admit limit */
    ai_file_t   lf;             /* post.zlp, held open for one pack */
    int         lf_pack;        /* which pack lf belongs to, -1 if none */

    /* shortlist, kept as a min-heap on score so the worst is cheap to
     * evict -- an array scan per candidate would be O(n*k) over the
     * whole corpus, which at 250k chunks is the difference between a
     * query and a nap. */
    ai_cand_t   shortlist[AI_SHORTLIST];
    int         nshort;

    ai_hit_t    hits[AI_HITS_MAX];
    int         nhits;
    int         want;
} ai_query_t;

/* -- API -- */

/* Finds and loads every pack under /ask. Returns how many loaded.
 * `progress` may be NULL; it is called with (done, total) in bytes
 * while reading, which is what the app's startup bar tracks. */
int  ai_load_all(const char *askroot,
                 void (*progress)(uint32_t done, uint32_t total,
                                  const char *what));
void ai_free_all(void);
int  ai_pack_count(void);
/* Why the last load found nothing, or why it found fewer packs than
 * are installed. Never NULL. */
const char *ai_error(void);
/* Packs that were present but did not load. Non-zero after a partial
 * load, where ai_pack_count() alone looks healthy. */
int  ai_failed_count(void);
const ai_pack_t *ai_pack(int i);
uint32_t ai_resident(void);

/* Starts a query. Returns false if nothing is loaded. */
bool ai_query_begin(ai_query_t *q, const char *text, int want);
/* Does a bounded amount of work. Returns true when the query is
 * finished (phase is AI_Q_DONE or AI_Q_CANCELLED). */
bool ai_query_step(ai_query_t *q);
void ai_query_cancel(ai_query_t *q);

/* Reads a passage's bytes. Returns the number read, or < 0. */
int  ai_preview(const ai_hit_t *h, char *buf, int maxlen);

/* Builds "<path>#<off>" for Z_WM_SET_ARG. Returns false if it would
 * not fit in AI_PATH_MAX, which the card layout is designed to make
 * impossible -- see docs/ask_app.md. */
bool ai_launch_arg(const ai_hit_t *h, char *buf, int buflen);

/* Browsing, for the TAB view: fills `out` with up to `max` documents
 * of pack `p` starting at `from`. Returns how many. */
int  ai_browse(const ai_pack_t *p, uint32_t from, int max,
               char titles[][AI_TITLE_MAX], char paths[][AI_PATH_MAX]);

#endif
