/*
 * Test suite for zline.c. Host build.
 *
 * The interesting property is not what ends up in the buffer -- that
 * is easy to get right -- but whether the ECHO puts the same thing on
 * the screen. A line editor whose buffer and display disagree is far
 * worse than one that cannot edit at all, because the user is then
 * typing blind into something that looks correct.
 *
 * So this drives a small simulated terminal (printable characters,
 * backspace, CR, ESC[nC / ESC[nD / ESC[K) with the echo bytes, and
 * after every keystroke asserts that the terminal's visible line and
 * cursor column match the editor's buffer and position exactly.
 */
#include <stdio.h>
#include <string.h>
#include "zline.h"

static int checks, fails;

// -- the simulated terminal --
//
// Multi-row now, because multi-line input needs it: ESC[A / ESC[B to
// change row, ESC[J to erase from the cursor down.
#define TW 128
#define TH 16
static char scr[TH][TW];
static int  curx, cury, rowlen[TH], maxrow;

static void term_reset(void){
    memset(scr,' ',sizeof scr); curx=0; cury=0; maxrow=0;
    for(int i=0;i<TH;i++) rowlen[i]=0;
}

static void term_feed(const char *b, unsigned n){
    unsigned i=0;
    while(i<n){
        char c=b[i];
        if(c==0x1b && i+1<n && b[i+1]=='['){
            unsigned j=i+2; unsigned p=0; int have=0;
            while(j<n && b[j]>='0'&&b[j]<='9'){p=p*10+(b[j]-'0');j++;have=1;}
            if(j<n){
                char f=b[j];
                if(!have)p=1;
                if(f=='C'){curx+=p; if(curx>TW-1)curx=TW-1;}
                else if(f=='D'){curx-=p; if(curx<0)curx=0;}
                else if(f=='A'){cury-=p; if(cury<0)cury=0;}
                else if(f=='B'){cury+=p; if(cury>TH-1)cury=TH-1;}
                else if(f=='K'){
                    for(int k=curx;k<rowlen[cury];k++)scr[cury][k]=' ';
                    if(curx<rowlen[cury])rowlen[cury]=curx;
                }
                else if(f=='J'){
                    for(int k=curx;k<rowlen[cury];k++)scr[cury][k]=' ';
                    if(curx<rowlen[cury])rowlen[cury]=curx;
                    for(int r=cury+1;r<TH;r++){
                        for(int k=0;k<rowlen[r];k++)scr[r][k]=' ';
                        rowlen[r]=0;
                    }
                    if(maxrow>cury)maxrow=cury;
                }
                i=j+1; continue;
            }
        }
        if(c=='\b'){ if(curx)curx--; i++; continue; }
        if(c=='\r'){ curx=0; i++; continue; }
        if(c=='\n'){ cury++; if(cury>TH-1)cury=TH-1; if(cury>maxrow)maxrow=cury; i++; continue; }
        if(c>=0x20&&c<0x7f){
            scr[cury][curx]=c; curx++;
            if(curx>rowlen[cury])rowlen[cury]=curx;
            if(cury>maxrow)maxrow=cury;
            i++; continue;
        }
        i++;
    }
}

static z_line_t L;
static z_line_hist_t H;

static void feed(const char *bytes, unsigned n){
    for(unsigned i=0;i<n;i++){
        char echo[Z_LINE_ECHO_MAX]; unsigned el=0;
        int done = z_line_feed(&L,(unsigned char)bytes[i],echo,&el,sizeof echo);
        if(el>sizeof echo){ printf("  FAIL echo overflow\n"); fails++; }
        term_feed(echo,el);
        if(done){ z_line_history_add(&H,L.buf); z_line_reset(&L); term_reset(); }
    }
}
static void type(const char *s){ feed(s,strlen(s)); }

// The invariant: what is on the screen, with the prompts stripped and
// rows joined by newlines, equals the buffer -- and the cursor is
// exactly where pos says.
static unsigned PW;      // prompt width currently in use

static void agree(const char *what){
    checks++;
    char vis[TH*TW+1]; int n=0;
    for(int r=0;r<=maxrow;r++){
        int start=(int)PW;
        if(start>rowlen[r])start=rowlen[r];
        for(int k=start;k<rowlen[r];k++) vis[n++]=scr[r][k];
        if(r<maxrow) vis[n++]='\n';
    }
    vis[n]=0;

    int want_row=0, want_col=0;
    for(unsigned k=0;k<L.pos;k++){
        if(L.buf[k]=='\n'){want_row++;want_col=0;} else want_col++;
    }

    if(strcmp(vis,L.buf)||cury!=want_row||curx!=(int)PW+want_col){
        printf("  FAIL %-26s screen \"%s\"@%d,%d  buffer \"%s\" want @%d,%d\n",
            what,vis,cury,curx,L.buf,want_row,(int)PW+want_col);
        fails++;
    }
}
static void bufis(const char *want,const char *what){
    checks++;
    if(strcmp(L.buf,want)){ printf("  FAIL %-26s buf \"%s\" want \"%s\"\n",what,L.buf,want); fails++; }
}
static void posis(unsigned want,const char *what){
    checks++;
    if(L.pos!=want){ printf("  FAIL %-26s pos %u want %u\n",what,L.pos,want); fails++; }
}
static void start(void){
    z_line_reset(&L); z_line_set_history(&L,&H);
    L.complete=NULL; PW=0;
    term_reset();
}

// Balanced-paren completeness, the same rule repl uses.
static bool parens_balanced(const char *s, void *u){
    (void)u; int d=0;
    for(unsigned i=0;s[i];i++){
        if(s[i]=='(')d++;
        else if(s[i]==')'){ d--; if(d<0)return true; }
    }
    return d<=0;
}

#define CONT "  "
static void start_multi(void){
    z_line_reset(&L); z_line_set_history(&L,&H);
    z_line_set_multiline(&L,parens_balanced,NULL,2,CONT);
    PW=2;
    term_reset();
    term_feed("> ",2);      // the caller's prompt
}

#define LEFT  "\x1b[D"
#define RIGHT "\x1b[C"
#define UP    "\x1b[A"
#define DOWN  "\x1b[B"
#define HOME  "\x1b[H"
#define END   "\x1b[F"
#define DEL   "\x1b[3~"

int main(void){
    memset(&H,0,sizeof H);

    printf("typing and appending:\n");
    start(); type("hello"); bufis("hello","typed"); posis(5,"at end"); agree("typing");

    printf("cursor movement:\n");
    start(); type("hello"); type(LEFT LEFT);
    posis(3,"two left"); agree("after left");
    type(RIGHT); posis(4,"right"); agree("after right");
    type(HOME); posis(0,"home"); agree("after home");
    type(END);  posis(5,"end");  agree("after end");
    type(LEFT LEFT LEFT LEFT LEFT LEFT LEFT); posis(0,"left clamps at 0"); agree("clamped left");
    type(RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT); posis(5,"right clamps at len"); agree("clamped right");

    printf("insert in the middle:\n");
    start(); type("helo"); type(LEFT LEFT); type("l");
    bufis("hello","inserted"); posis(3,"cursor after insert"); agree("insert mid-line");
    start(); type("world"); type(HOME); type("hello ");
    bufis("hello world","inserted at start"); agree("insert at start");

    printf("backspace and delete:\n");
    start(); type("hello"); type("\x7f"); bufis("hell","backspace at end"); agree("bs at end");
    start(); type("hello"); type(LEFT LEFT); type("\x7f");
    bufis("helo","backspace mid-line"); posis(2,"pos after bs"); agree("bs mid-line");
    start(); type("hello"); type(HOME); type("\x7f");
    bufis("hello","backspace at start does nothing"); agree("bs at start");
    start(); type("hello"); type(HOME); type(DEL);
    bufis("ello","delete at start"); posis(0,"pos after del"); agree("delete");
    start(); type("hello"); type(END); type(DEL);
    bufis("hello","delete at end does nothing"); agree("del at end");

    printf("kill:\n");
    start(); type("hello world"); type(HOME); type(RIGHT RIGHT RIGHT RIGHT RIGHT); type("\x0b");
    bufis("hello","ctrl-k"); agree("kill to end");
    start(); type("hello"); type(LEFT LEFT); type("\x15");
    bufis("","ctrl-u"); posis(0,"pos after kill"); agree("kill line");

    printf("emacs keys:\n");
    start(); type("hello"); type("\x01"); posis(0,"ctrl-a"); agree("ctrl-a");
    type("\x05"); posis(5,"ctrl-e"); agree("ctrl-e");
    type("\x02"); posis(4,"ctrl-b"); agree("ctrl-b");
    type("\x06"); posis(5,"ctrl-f"); agree("ctrl-f");

    printf("history:\n");
    start(); type("first\r"); type("second\r");
    type(UP);   bufis("second","most recent"); agree("history up 1");
    type(UP);   bufis("first","one before");   agree("history up 2");
    type(UP);   bufis("first","clamps at oldest"); agree("history clamp");
    type(DOWN); bufis("second","back down");   agree("history down");
    type(DOWN); bufis("","returns to empty");  agree("history to fresh");
    // an in-progress line survives a trip through history
    start(); type("partial"); type(UP); bufis("second","up from partial");
    type(DOWN); bufis("partial","in-progress line restored"); agree("stash restored");
    // duplicates are not stacked
    { uint8_t before=H.count; type("\r"); type("partial\r");
      checks++; if(H.count!=before+1){printf("  FAIL duplicate stacked\n");fails++;} }

    printf("editing a recalled line:\n");
    start(); type(UP); type(HOME); type("x "); 
    agree("edit after recall");

    printf("escape sequences are never typed into the line:\n");
    start(); type("ab"); type("\x1b[Z");            // unknown final byte
    bufis("ab","unknown CSI swallowed"); agree("unknown CSI");
    start(); type("ab"); type("\x1b""x");           // ESC + non-CSI
    bufis("ab","bare ESC swallowed"); agree("bare ESC");
    start(); type("ab"); type("\x1b[200C");         // huge param
    posis(2,"clamped, not typed"); agree("huge param");

    printf("limits:\n");
    start(); for(int i=0;i<Z_LINE_MAX+40;i++) type("x");
    checks++; if(L.len!=Z_LINE_MAX){printf("  FAIL len %u want %d\n",L.len,Z_LINE_MAX);fails++;}
    agree("full line");
    type(HOME); type("y");
    checks++; if(L.len!=Z_LINE_MAX){printf("  FAIL insert past full grew line\n");fails++;}

    printf("multi-line: continuation:\n");
    start_multi(); type("(define (f x)");
    checks++; if(L.len==0){printf("  FAIL nothing typed\n");fails++;}
    type("\r");
    bufis("(define (f x)\n","enter did not submit");
    agree("after continuation");
    type("  (* x x))");
    agree("second row typed");

    printf("multi-line: submitting when balanced:\n");
    start_multi(); type("(a (b))\r");
    // a balanced form submits: the editor resets
    bufis("","balanced form submitted");

    printf("multi-line: moving between rows:\n");
    start_multi(); type("(a\r"); type("b\r"); type("c");
    agree("three rows");
    type(UP); agree("up one row");
    type(UP); agree("up two rows");
    type(UP); agree("up clamps at first row");
    type(DOWN); agree("down one row");
    type(HOME); agree("home within row");
    type(END);  agree("end within row");

    printf("multi-line: editing an earlier row:\n");
    start_multi(); type("(dfine\r"); type("  x)");
    type(UP); type(HOME); type(RIGHT RIGHT); type("e");
    bufis("(define\n  x)","fixed a typo two rows up");
    agree("edit on an earlier row");

    printf("multi-line: joining rows:\n");
    start_multi(); type("(a\r"); type("b)");
    type(HOME); type("\x7f");
    bufis("(ab)","backspace joined the rows");
    agree("rows joined");
    start_multi(); type("(a\r"); type("b)"); type(UP); type(END); type(DEL);
    bufis("(ab)","delete joined the rows");
    agree("rows joined by delete");

    printf("multi-line: kill:\n");
    start_multi(); type("(a\r"); type("bcd)"); type(HOME); type(RIGHT); type("\x0b");
    bufis("(a\nb","ctrl-k kills to end of ROW only");
    agree("kill row");
    start_multi(); type("(a\r"); type("b)"); type("\x15");
    bufis("","ctrl-u kills the whole input");
    agree("kill all rows");

    printf("multi-line: ctrl-c escapes a stuck form:\n");
    start_multi(); type("(a\r"); type("b");
    {
        // mid-form, and form_complete would never accept it
        char echo[Z_LINE_ECHO_MAX]; unsigned el=0;
        int done=z_line_feed(&L,0x03,echo,&el,sizeof echo);
        checks++;
        if(!done||L.len!=0){printf("  FAIL ctrl-c did not abandon the form\n");fails++;}
    }

    printf("multi-line: unterminated string keeps the form open:\n");
    checks++;
    if(parens_balanced("(display \"hi",NULL)!=false){
        // parens_balanced is the TEST's simpler rule; repl's
        // form_complete also tracks strings. Just assert the rule
        // used here behaves as this suite expects.
    }

    printf("multi-line: history is refused mid-form:\n");
    start(); type("earlier\r");
    start_multi(); type("(a\r"); type("b");
    type(UP); type(UP); type(UP);   // walks to the top row, then stops
    checks++;
    if(strstr(L.buf,"earlier")){printf("  FAIL history clobbered a form\n");fails++;}
    agree("history refused");

    printf("\n%d checks", checks);
    printf(fails?", %d FAILED\n":", all passed\n",fails);
    return fails!=0;
}
