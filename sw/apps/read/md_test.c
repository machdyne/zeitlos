/*
 * Test suite for md.c. Host build:  make test
 *
 * Two halves. The first is a set of specific constructs with expected
 * output, which is where bugs get diagnosed. The second runs the
 * parser over whole real documents and asserts invariants -- no
 * crashes, no runaway spans, no lost text -- which is where bugs get
 * FOUND, because real documents contain things nobody thinks to write
 * a case for.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "md.h"

static int checks, fails;

static const char *kindname(md_kind_t k) {
    switch (k) {
        case MD_BLANK: return "blank"; case MD_SKIP: return "skip";  case MD_PARA: return "para";
        case MD_HEADING: return "head"; case MD_CODE: return "code";
        case MD_QUOTE: return "quote";  case MD_LIST: return "list";
        case MD_RULE: return "rule";    case MD_TABLE: return "table";
    }
    return "?";
}

// one line, fresh state
static void one(const char *src, md_kind_t kind, const char *text) {
    md_state_t st; md_state_init(&st);
    md_line_t o;
    md_parse(&st, src, NULL, &o);
    checks++;
    if (o.kind != kind || strcmp(o.text, text)) {
        printf("  FAIL  %-30s got %s \"%s\"  want %s \"%s\"\n",
            src, kindname(o.kind), o.text, kindname(kind), text);
        fails++;
    }
}

static void spans(const char *src, int nspans, int nlinks) {
    md_state_t st; md_state_init(&st);
    md_line_t o;
    md_parse(&st, src, NULL, &o);
    checks++;
    if (o.nspans != nspans || o.nlinks != nlinks) {
        printf("  FAIL  %-30s got %d spans %d links  want %d/%d\n",
            src, o.nspans, o.nlinks, nspans, nlinks);
        fails++;
    }
}

// a row laid out at `chars`: its display lines joined with '|'
static void layout(const char *src, int chars, const char *want) {
    md_state_t st; md_state_init(&st);
    md_line_t o;
    md_parse(&st, src, NULL, &o);
    md_table_layout(&o, chars);
    char got[MD_LINE_MAX * 2];
    int L = md_table_line_len(&o), n = 0;
    for (int i = 0; i < o.len; i++) {
        if (i && i % L == 0) got[n++] = '|';
        got[n++] = o.text[i];
    }
    got[n] = 0;
    checks++;
    if (strcmp(got, want) || o.len % L) {
        printf("  FAIL  %-30s @%d got \"%s\"  want \"%s\"\n", src, chars, got, want);
        fails++;
    }
}

// a table row's cells, joined with '/'
static void cells(const char *src, const char *want) {
    md_state_t st; md_state_init(&st);
    md_line_t o;
    md_parse(&st, src, NULL, &o);
    char got[MD_LINE_MAX * 2];
    int n = 0;
    for (int c = 0; c < o.ncells; c++) {
        if (c) got[n++] = '/';
        for (int i = o.cell[c]; i < o.cell[c + 1]; i++) got[n++] = o.text[i];
    }
    got[n] = 0;
    checks++;
    if (o.kind != MD_TABLE || strcmp(got, want)) {
        printf("  FAIL  %-30s got %s \"%s\"  want \"%s\"\n",
            src, kindname(o.kind), got, want);
        fails++;
    }
}

int main(int argc, char **argv) {

    printf("headings:\n");
    one("# Title", MD_HEADING, "Title");
    one("### Deep", MD_HEADING, "Deep");
    one("####### Seven", MD_PARA, "####### Seven");   // 7 is not a heading
    one("#NoSpace", MD_PARA, "#NoSpace");
    one("## Closed ##", MD_HEADING, "Closed");

    printf("rules and lists:\n");
    one("***", MD_RULE, "");
    // "---" on the FIRST line is front matter, not a rule -- tc.md
    // opens with exactly that. Once anything else has been seen it
    // is a rule; the front-matter block below tests that transition.
    {
        md_state_t st; md_state_init(&st); md_line_t o;
        md_parse(&st, "text", NULL, &o);
        md_parse(&st, "---", NULL, &o);
        checks++;
        if (o.kind != MD_RULE) { printf("  FAIL --- after text is a rule\n"); fails++; }
    }
    one("- item", MD_LIST, "item");
    one("* item", MD_LIST, "item");
    one("1. first", MD_LIST, "first");
    one("12) twelfth", MD_LIST, "twelfth");
    one("-notalist", MD_PARA, "-notalist");

    printf("quotes, tables, code:\n");
    one("> quoted", MD_QUOTE, "quoted");
    one(">quoted", MD_QUOTE, "quoted");
    one("| a | b |", MD_TABLE, "ab");
    cells("| a | b |", "a/b");
    cells("|a|b", "a/b");                        // trailing pipe optional
    cells("| `x \\| y` | z |", "x | y/z");     // escaped pipe, in code too
    cells("| ls \\| wc |", "ls | wc");
    cells("| **bold** | [l](u) |", "bold/l");   // inline syntax per cell
    cells("| | b |", "/b");                      // an empty cell
    cells("|", "");
    cells("| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |",
        "1/2/3/4/5/6/7/8/9/10 | 11");            // the rest in the last
    spans("| `a` | [b](c) |", 2, 1);
    {
        // a table, with the reader's lookahead done by hand
        md_state_t st; md_state_init(&st); md_line_t o;
        md_parse(&st, "| h | head2 |", "|---|---|", &o);
        checks++; if (o.tflags != (MD_TF_FIRST | MD_TF_HEAD) || o.tcols != 2 ||
            o.colw[0] != 1 || o.colw[1] != 5) {
            printf("  FAIL header row flags %x cols %d w %d %d\n", o.tflags,
                o.tcols, o.colw[0], o.colw[1]); fails++; }
        uint8_t w[MD_TBL_COLS], mw[MD_TBL_COLS];
        int n = md_table_measure("| wider | x | extra |", w, mw);
        checks++; if (n != 3 || w[0] != 5 || md_table_measure("|--|", w, mw) != -1 ||
            md_table_measure("text", w, mw) != 0) {
            printf("  FAIL md_table_measure\n"); fails++; }
        n = md_table_measure("| two words | x | a longerword |", w, mw);
        checks++; if (n != 3 || w[0] != 9 || mw[0] != 5 || mw[2] != 10) {
            printf("  FAIL md_table_measure words %d %d\n", mw[0], mw[2]); fails++; }
        n = md_table_measure("| wider | x | extra |", w, mw);
        md_table_widen(&st, &o, w, mw, n);
        checks++; if (o.tcols != 3 || o.colw[0] != 5 || o.colw[1] != 5 ||
            st.tbl_w[2] != 5) { printf("  FAIL md_table_widen\n"); fails++; }
        md_parse(&st, "|---|---|", "| a | b |", &o);
        md_parse(&st, "| a | b |", "| c | d |", &o);
        checks++; if (o.tflags != MD_TF_BODY1 || o.tcols != 3 || o.colw[0] != 5) {
            printf("  FAIL first body row %x\n", o.tflags); fails++; }
        md_parse(&st, "| c | d |", "after", &o);
        checks++; if (o.tflags != MD_TF_LAST || st.tbl_cols) {
            printf("  FAIL last row %x\n", o.tflags); fails++; }
        md_parse(&st, "| new |", NULL, &o);
        checks++; if (o.tflags != (MD_TF_FIRST | MD_TF_LAST) || o.colw[0] != 3) {
            printf("  FAIL second table %x\n", o.tflags); fails++; }
        // a table cut off by a paragraph the caller did not peek
        md_parse(&st, "| a |", "| b |", &o);
        md_parse(&st, "para", NULL, &o);
        checks++; if (st.tbl_cols) { printf("  FAIL table not ended\n"); fails++; }
    }
    one("|---|---|", MD_SKIP, "");
    one("    indented", MD_CODE, "indented");

    printf("inline:\n");
    one("plain text", MD_PARA, "plain text");
    one("a `code` b", MD_PARA, "a code b");
    one("**bold** text", MD_PARA, "bold text");
    one("*ital* text", MD_PARA, "ital text");
    one("[link](http://x)", MD_PARA, "link");
    one("![alt](img.png)", MD_PARA, "alt");
    one("a `unclosed", MD_PARA, "a `unclosed");
    one("snake_case_name", MD_PARA, "snake_case_name");
    one("2 * 3 * 4", MD_PARA, "2 * 3 * 4");
    one("_real emphasis_", MD_PARA, "real emphasis");
    one("a_b_c and d_e", MD_PARA, "a_b_c and d_e");
    // Pandoc attribute syntax is NOT stripped -- it is not GitHub
    // Markdown, and a rule to remove it would eat legitimate braces.
    one("Heading {#anchor}", MD_PARA, "Heading {#anchor}");
    one("a ::: b", MD_PARA, "a ::: b");
    one("a <b>tag</b> c", MD_PARA, "a tag c");
    one("a \\* b", MD_PARA, "a * b");
    one("less a < b more", MD_PARA, "less a < b more");

    spans("a `x` b", 1, 0);
    spans("[a](u) and [b](v)", 2, 2);
    spans("`a` `b` `c`", 3, 0);
    spans("no spans here", 0, 0);

    printf("fences suppress markup:\n");
    {
        md_state_t st; md_state_init(&st); md_line_t o;
        md_parse(&st, "```c", NULL, &o);
        checks++; if (o.kind != MD_SKIP) { printf("  FAIL fence open\n"); fails++; }
        md_parse(&st, "# not a heading", NULL, &o);
        checks++; if (o.kind != MD_CODE || strcmp(o.text, "# not a heading")) {
            printf("  FAIL inside fence: %s \"%s\"\n", kindname(o.kind), o.text); fails++; }
        md_parse(&st, "```", NULL, &o);
        checks++; if (o.kind != MD_SKIP || st.in_fence) { printf("  FAIL fence close\n"); fails++; }
        md_parse(&st, "# heading again", NULL, &o);
        checks++; if (o.kind != MD_HEADING) { printf("  FAIL after fence\n"); fails++; }
    }

    printf("front matter only at the top:\n");
    {
        md_state_t st; md_state_init(&st); md_line_t o;
        md_parse(&st, "---", "title: x", &o);
        checks++; if (!st.in_front) { printf("  FAIL front open\n"); fails++; }
        md_parse(&st, "title: x", NULL, &o);
        checks++; if (o.kind != MD_SKIP) { printf("  FAIL inside front\n"); fails++; }
        md_parse(&st, "---", NULL, &o);
        checks++; if (st.in_front) { printf("  FAIL front close\n"); fails++; }
        md_parse(&st, "# Real", NULL, &o);
        checks++; if (o.kind != MD_HEADING) { printf("  FAIL after front\n"); fails++; }
        // a --- later in the document is a rule, not front matter
        md_parse(&st, "---", NULL, &o);
        checks++; if (o.kind != MD_RULE || st.in_front) {
            printf("  FAIL later --- became front matter\n"); fails++; }
    }

    // A --- on the first line with no `key:` after it is a rule, and
    // front matter that never closes ends after MD_FRONT_MAX lines.
    // Found by fuzzing (front_unterminated.md): the whole document
    // was one front matter block, a blank page.
    printf("front matter needs a key, and ends:\n");
    {
        md_state_t st; md_state_init(&st); md_line_t o;
        md_parse(&st, "---", "", &o);
        checks++; if (st.in_front || o.kind != MD_RULE) {
            printf("  FAIL --- then blank: %s, in_front %d\n", kindname(o.kind), st.in_front); fails++; }
        md_state_init(&st);
        md_parse(&st, "---", "# Title", &o);
        checks++; if (st.in_front) { printf("  FAIL --- then heading opened front\n"); fails++; }
        md_state_init(&st);
        md_parse(&st, "---", NULL, &o);
        checks++; if (st.in_front) { printf("  FAIL lone --- opened front\n"); fails++; }
        md_state_init(&st);
        md_parse(&st, "---", "---", &o);
        checks++; if (!st.in_front) { printf("  FAIL empty front matter\n"); fails++; }
        md_state_init(&st);
        md_parse(&st, "---", "title: x", &o);
        int n = 1;
        while (st.in_front && n < 1000) { md_parse(&st, "title: more", NULL, &o); n++; }
        checks++; if (st.in_front || n > MD_FRONT_MAX + 1) {
            printf("  FAIL unclosed front matter ran %d lines\n", n); fails++; }
        md_parse(&st, "# After", NULL, &o);
        checks++; if (o.kind != MD_HEADING) { printf("  FAIL after capped front\n"); fails++; }
    }

    // CommonMark 4.5: a backtick fence's info string may not contain a
    // backtick. docs/learn_format.md opens a paragraph with
    // ```` ```figure ```` and the rest of it became code.
    printf("fences and backtick runs:\n");
    one("```` ```figure ```` blocks", MD_PARA, "```figure blocks");
    one("``` a`b", MD_PARA, "``` a`b");
    one("`` a`b ``", MD_PARA, "a`b");
    one("``x` y``", MD_PARA, "x` y");
    {
        md_state_t st; md_state_init(&st); md_line_t o;
        md_parse(&st, "``` a`b", NULL, &o);
        checks++; if (st.in_fence) { printf("  FAIL backtick in info opened a fence\n"); fails++; }
        md_parse(&st, "~~~ a`b", NULL, &o);
        checks++; if (!st.in_fence) { printf("  FAIL tilde fence with ` in info\n"); fails++; }
        md_state_init(&st);
        checks++; if (!md_continues(&st, MD_PARA, "``` a`b")) {
            printf("  FAIL ``` a`b ended a paragraph\n"); fails++; }
    }

    // A run of lines that draw nothing reports a blank every
    // MD_SKIP_RUN of them, so the reader's layout cannot walk the
    // whole run to fill a screen (fuzz case skiprows.md).
    printf("long runs of MD_SKIP are bounded:\n");
    {
        md_state_t st; md_state_init(&st); md_line_t o;
        md_parse(&st, "| a | b |", NULL, &o);
        int run = 0, maxrun = 0;
        for (int i = 0; i < 5000; i++) {
            md_parse(&st, "|---|---|", NULL, &o);
            if (o.kind == MD_SKIP) { if (++run > maxrun) maxrun = run; }
            else run = 0;
        }
        checks++; if (maxrun >= MD_SKIP_RUN) {
            printf("  FAIL %d MD_SKIP lines in a row\n", maxrun); fails++; }
        // and an ordinary table keeps its separator invisible
        md_state_init(&st);
        md_parse(&st, "| a |", NULL, &o);
        md_parse(&st, "|---|", NULL, &o);
        checks++; if (o.kind != MD_SKIP) { printf("  FAIL separator row drew\n"); fails++; }
    }

    printf("table layout:\n");
    layout("| a | b |", 40, " a b ");
    layout("| one two three | x |", 11, " one two x | three     ");
    layout("| `code here` | y |", 9, " code  y | here    ");
    layout("| a | b | c |", 6, " a b c ");    // too narrow: clipped, not shredded
    // keys keep their width; the prose column takes what is left
    layout("| key | some words of prose here |", 22, " key some words of    |     prose here       ");
    layout("| a |", 40, " a ");
    {
        // spans follow their characters onto the next line
        md_state_t st; md_state_init(&st); md_line_t o;
        md_parse(&st, "| `ab cd` | y |", NULL, &o);
        md_table_layout(&o, 7);
        checks++; if (o.nspans != 2 || o.spans[0].start != 1 || o.spans[0].len != 2 ||
            o.spans[1].start != 8 || o.spans[1].len != 2) {
            printf("  FAIL layout spans %d [%d %d]\n", o.nspans,
                o.spans[0].start, o.spans[0].len); fails++; }
    }

    printf("setext headings:\n");
    {
        md_state_t st; md_state_init(&st); md_line_t o;
        bool ate = md_parse(&st, "Title", "=====", &o);
        checks++; if (o.kind != MD_HEADING || o.level != 1 || !ate) {
            printf("  FAIL setext h1\n"); fails++; }
        ate = md_parse(&st, "Sub", "-----", &o);
        checks++; if (o.kind != MD_HEADING || o.level != 2 || !ate) {
            printf("  FAIL setext h2\n"); fails++; }
        ate = md_parse(&st, "Para", "more text", &o);
        checks++; if (o.kind != MD_PARA || ate) { printf("  FAIL non-setext\n"); fails++; }
    }

    // -- second half: run over whole real documents --
    if (argc > 1) {
        printf("\nreal documents:\n");
        for (int a = 1; a < argc; a++) {
            FILE *f = fopen(argv[a], "rb");
            if (!f) { printf("  (skip %s)\n", argv[a]); continue; }
            static char buf[1 << 20];
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0; fclose(f);

            md_state_t st; md_state_init(&st);
            md_line_t o;
            int lines = 0, kinds[9] = {0}, spanmax = 0, linkmax = 0;
            int skiprun = 0, skipmax = 0;
            size_t textbytes = 0;

            char *p = buf;
            while (*p) {
                char *e = strchr(p, '\n');
                if (e) *e = 0;
                char *nx = e ? e + 1 : NULL;
                char *nxe = NULL;
                if (nx) { nxe = strchr(nx, '\n'); if (nxe) *nxe = 0; }

                bool ate = md_parse(&st, p, nx, &o);

                lines++;
                if (o.kind < 9) kinds[o.kind]++;
                // Front matter is bounded by MD_FRONT_MAX, not by this.
                if (o.kind == MD_SKIP && !st.in_front) { if (++skiprun > skipmax) skipmax = skiprun; }
                else if (o.kind != MD_SKIP) skiprun = 0;
                if (o.nspans > spanmax) spanmax = o.nspans;
                if (o.nlinks > linkmax) linkmax = o.nlinks;
                textbytes += o.len;

                // invariants
                checks++;
                if (o.len >= MD_LINE_MAX) { printf("  FAIL %s: len overflow\n", argv[a]); fails++; }
                if (o.text[o.len] != 0) { printf("  FAIL %s: not terminated\n", argv[a]); fails++; }
                for (int s = 0; s < o.nspans; s++) {
                    if (o.spans[s].start + o.spans[s].len > o.len) {
                        printf("  FAIL %s line %d: span past end\n", argv[a], lines); fails++; }
                    if (o.spans[s].kind == MD_SPAN_LINK && o.spans[s].link >= o.nlinks) {
                        printf("  FAIL %s line %d: span links to nothing\n", argv[a], lines); fails++; }
                }
                if (nxe) *nxe = '\n';
                if (e) *e = '\n';
                p = e ? e + 1 : p + strlen(p);
                if (ate && p && *p) { char *e2 = strchr(p, '\n'); p = e2 ? e2 + 1 : p + strlen(p); }
            }

            const char *base = strrchr(argv[a], '/');
            printf("  %-24s %5d lines  para %d head %d code %d list %d table %d quote %d"
                   "  maxspans %d maxlinks %d\n",
                base ? base + 1 : argv[a], lines,
                kinds[MD_PARA], kinds[MD_HEADING], kinds[MD_CODE],
                kinds[MD_LIST], kinds[MD_TABLE], kinds[MD_QUOTE],
                spanmax, linkmax);
            if (st.in_fence) { printf("  FAIL %s: ended inside a fence\n", argv[a]); fails++; }
            if (st.in_front) { printf("  FAIL %s: ended inside front matter\n", argv[a]); fails++; }
            if (skipmax >= MD_SKIP_RUN) {
                printf("  FAIL %s: %d lines in a row draw nothing\n", argv[a], skipmax); fails++; }
        }
    }

    printf("\n%d checks", checks);
    printf(fails ? ", %d FAILED\n" : ", all passed\n", fails);
    return fails != 0;
}
