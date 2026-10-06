/*
 * 2048 Game - Web Server in C
 * Cross-platform (Windows & Linux)
 *
 * Compile:
 *   Linux:   gcc -std=c99 -o 2048 2048.c
 *   Windows (MSVC): cl 2048.c
 *   Windows (MinGW): gcc -o 2048.exe 2048.c -lws2_32
 *
 * Run: ./2048  (or 2048.exe on Windows)
 * Open browser to http://localhost:8080
 * Use WASD / Arrow keys to play
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
#define CLOSE_SOCK    closesocket
#define VALID_SOCK(s) ((s) != INVALID_SOCKET)
#define BAD_SOCK      INVALID_SOCKET
#define SOCK_ERR      SOCKET_ERROR
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <signal.h>
typedef int sock_t;
#define CLOSE_SOCK    close
#define VALID_SOCK(s) ((s) >= 0)
#define BAD_SOCK      (-1)
#define SOCK_ERR      (-1)
#endif

/* ---------- constants ---------- */
#define N          4
#define MAX_UNDO   3
#define PORT       8080
#define BUF_SZ     32768
#define SAVE_FILE "2048_save.dat"

/* ---------- types ---------- */
typedef struct { int c[N][N]; } Board;

typedef struct { Board b; int sc; } Snap;

typedef struct {
    Board  b;
    int    sc;
    int    over;
    int    won;
    Snap   hist[MAX_UNDO];
    int    hc;
} Game;

/* ---------- globals ---------- */
static int    running = 1;
static sock_t g_server = BAD_SOCK;

/* ==================================================================
 *  game logic
 * ================================================================*/

static void add_tile(Board *b) {
    int empty[N * N][2], cnt = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            if (b->c[i][j] == 0) { empty[cnt][0] = i; empty[cnt][1] = j; cnt++; }
    if (cnt == 0) return;
    int idx = rand() % cnt;
    b->c[empty[idx][0]][empty[idx][1]] = (rand() % 10 < 9) ? 2 : 4;
}

static void init_game(Game *g) {
    memset(&g->b, 0, sizeof(Board));
    g->sc = g->over = g->won = 0;
    g->hc = 0;
    add_tile(&g->b);
    add_tile(&g->b);
}

/* slide one row left, return merge score */
static int slide_row(int row[N]) {
    int compact[N] = {0}, cpos = 0;
    for (int i = 0; i < N; i++)
        if (row[i] != 0) compact[cpos++] = row[i];

    int merged[N] = {0}, mpos = 0, score = 0;
    for (int i = 0; i < cpos; i++) {
        if (i + 1 < cpos && compact[i] == compact[i + 1]) {
            merged[mpos] = compact[i] * 2;
            score += merged[mpos];
            i++; /* skip paired tile (each tile merges at most once) */
        } else {
            merged[mpos] = compact[i];
        }
        mpos++;
    }

    memset(row, 0, sizeof(int) * N);
    memcpy(row, merged, sizeof(int) * mpos);
    return score;
}

static void rev_row(int row[N]) {
    for (int i = 0; i < N / 2; i++) {
        int t = row[i]; row[i] = row[N - 1 - i]; row[N - 1 - i] = t;
    }
}

static void transpose(Board *b) {
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++) {
            int t = b->c[i][j]; b->c[i][j] = b->c[j][i]; b->c[j][i] = t;
        }
}

/* slide all rows left, return total merge score */
static int move_left(Board *b) {
    int s = 0;
    for (int i = 0; i < N; i++) s += slide_row(b->c[i]);
    return s;
}

static int move_right(Board *b) {
    int s = 0;
    for (int i = 0; i < N; i++) { rev_row(b->c[i]); s += slide_row(b->c[i]); rev_row(b->c[i]); }
    return s;
}

static int move_up(Board *b) {
    transpose(b); int s = move_left(b); transpose(b); return s;
}

static int move_down(Board *b) {
    transpose(b); int s = move_right(b); transpose(b); return s;
}

static int board_eq(Board *a, Board *b) {
    return memcmp(a, b, sizeof(Board)) == 0;
}

static int try_move(Game *g, const char *dir) {
    Board old = g->b;
    int old_sc = g->sc;
    int gain = 0;

    if      (strcmp(dir, "left")  == 0) gain = move_left(&g->b);
    else if (strcmp(dir, "right") == 0) gain = move_right(&g->b);
    else if (strcmp(dir, "up")    == 0) gain = move_up(&g->b);
    else if (strcmp(dir, "down")  == 0) gain = move_down(&g->b);
    else return 0;

    if (board_eq(&g->b, &old)) return 0;

    /* save undo snapshot */
    if (g->hc < MAX_UNDO) {
        g->hist[g->hc].b  = old;
        g->hist[g->hc].sc = old_sc;
        g->hc++;
    } else {
        memmove(&g->hist[0], &g->hist[1], (MAX_UNDO - 1) * sizeof(Snap));
        g->hist[MAX_UNDO - 1].b  = old;
        g->hist[MAX_UNDO - 1].sc = old_sc;
    }

    g->sc += gain;
    add_tile(&g->b);

    /* check win */
    if (!g->won)
        for (int i = 0; i < N && !g->won; i++)
            for (int j = 0; j < N && !g->won; j++)
                if (g->b.c[i][j] >= 2048) g->won = 1;

    /* check game over */
    g->over = 1;
    for (int i = 0; i < N && g->over; i++)
        for (int j = 0; j < N && g->over; j++) {
            if (g->b.c[i][j] == 0)                        { g->over = 0; }
            if (j + 1 < N && g->b.c[i][j] == g->b.c[i][j+1]) { g->over = 0; }
            if (i + 1 < N && g->b.c[i][j] == g->b.c[i+1][j]) { g->over = 0; }
        }

    return 1;
}

static int do_undo(Game *g) {
    if (g->hc == 0) return 0;
    g->hc--;
    g->b   = g->hist[g->hc].b;
    g->sc  = g->hist[g->hc].sc;
    g->over = 0;
    return 1;
}

/* ---------- save / load ---------- */
static int save_game(Game *g) {
    FILE *f = fopen(SAVE_FILE, "wb");
    if (!f) return 0;
    fwrite(&g->b,    sizeof(Board),     1, f);
    fwrite(&g->sc,   sizeof(int),       1, f);
    fwrite(&g->over, sizeof(int),       1, f);
    fwrite(&g->won,  sizeof(int),       1, f);
    fwrite(g->hist,  sizeof(Snap), MAX_UNDO, f);
    fwrite(&g->hc,   sizeof(int),       1, f);
    fclose(f);
    return 1;
}

static int load_game(Game *g) {
    FILE *f = fopen(SAVE_FILE, "rb");
    if (!f) return 0;
    fread(&g->b,    sizeof(Board),     1, f);
    fread(&g->sc,   sizeof(int),       1, f);
    fread(&g->over, sizeof(int),       1, f);
    fread(&g->won,  sizeof(int),       1, f);
    fread(g->hist,  sizeof(Snap), MAX_UNDO, f);
    fread(&g->hc,   sizeof(int),       1, f);
    fclose(f);
    return 1;
}

/* ==================================================================
 *  JSON response builder
 * ================================================================*/

static void json_state(Game *g, char *out, size_t size, const char *msg) {
    char buf[8192];
    int pos = 0;
    pos += sprintf(buf + pos, "{\"board\":[");
    for (int i = 0; i < N; i++) {
        pos += sprintf(buf + pos, "[");
        for (int j = 0; j < N; j++) {
            pos += sprintf(buf + pos, "%d", g->b.c[i][j]);
            if (j < N - 1) pos += sprintf(buf + pos, ",");
        }
        pos += sprintf(buf + pos, "]");
        if (i < N - 1) pos += sprintf(buf + pos, ",");
    }
    pos += sprintf(buf + pos,
        "],\"score\":%d,\"gameover\":%s,\"won\":%s,\"undocount\":%d,\"message\":\"%s\"}",
        g->sc,
        g->over ? "true" : "false",
        g->won  ? "true" : "false",
        g->hc,
        msg ? msg : "");
    strncpy(out, buf, size);
    out[size - 1] = '\0';
}

/* ==================================================================
 *  HTTP server
 * ================================================================*/

static void send_res(sock_t c, int code, const char *status,
                     const char *ctype, const char *body) {
    char buf[BUF_SZ];
    int len = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        code, status, ctype, strlen(body), body);
    send(c, buf, len, 0);
}

static void handle_client(sock_t client, Game *state) {
    char buf[BUF_SZ];
    memset(buf, 0, sizeof(buf));
    int n = recv(client, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { CLOSE_SOCK(client); return; }
    buf[n] = '\0';

    char method[16] = {0}, path[1024] = {0};
    sscanf(buf, "%15s %1023s", method, path);

    char json[8192];

    if (strcmp(path, "/") == 0 && strcmp(method, "GET") == 0) {
        extern const char *html_page;
        send_res(client, 200, "OK", "text/html; charset=utf-8", html_page);
        CLOSE_SOCK(client);
        return;
    }

    if (strcmp(path, "/state") == 0) {
        json_state(state, json, sizeof(json), "");
        send_res(client, 200, "OK", "application/json", json);
        CLOSE_SOCK(client);
        return;
    }

    if (strcmp(path, "/move") == 0 && strcmp(method, "POST") == 0) {
        char *body = strstr(buf, "\r\n\r\n");
        char dir[16] = {0};
        if (body && sscanf(body + 4, "dir=%15s", dir) == 1)
            try_move(state, dir);
        json_state(state, json, sizeof(json), "");
        send_res(client, 200, "OK", "application/json", json);
        CLOSE_SOCK(client);
        return;
    }

    if (strcmp(path, "/undo") == 0) {
        if (!do_undo(state))
            json_state(state, json, sizeof(json), "No moves to undo");
        else
            json_state(state, json, sizeof(json), "Undo successful");
        send_res(client, 200, "OK", "application/json", json);
        CLOSE_SOCK(client);
        return;
    }

    if (strcmp(path, "/save") == 0) {
        if (save_game(state))
            sprintf(json, "{\"success\":true,\"message\":\"Game saved\"}");
        else
            sprintf(json, "{\"success\":false,\"message\":\"Save failed\"}");
        send_res(client, 200, "OK", "application/json", json);
        CLOSE_SOCK(client);
        return;
    }

    if (strcmp(path, "/new") == 0) {
        init_game(state);
        json_state(state, json, sizeof(json), "New game started");
        send_res(client, 200, "OK", "application/json", json);
        CLOSE_SOCK(client);
        return;
    }

    if (strcmp(path, "/exit") == 0) {
        running = 0;
        sprintf(json, "{\"success\":true,\"message\":\"Server shutting down\"}");
        send_res(client, 200, "OK", "application/json", json);
        CLOSE_SOCK(client);
        if (VALID_SOCK(g_server)) { CLOSE_SOCK(g_server); g_server = BAD_SOCK; }
        return;
    }

    /* 404 fallback */
    send_res(client, 404, "Not Found", "text/plain", "404 Not Found");
    CLOSE_SOCK(client);
}

/* ==================================================================
 *  HTML page embedded
 * ================================================================*/

const char *html_page =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"UTF-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n"
    "<title>2048</title>\n"
    "<style>\n"
    "*{margin:0;padding:0;box-sizing:border-box}\n"
    "body{font-family:Arial,sans-serif;background:#faf8ef;display:flex;justify-content:center;align-items:center;min-height:100vh}\n"
    "#app{width:500px;padding:20px}\n"
    "#head{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px}\n"
    "h1{font-size:48px;color:#776e65}\n"
    "#scbox{background:#bbada0;padding:8px 20px;border-radius:6px;color:#fff;text-align:center;font-weight:bold}\n"
    "#scbox div:first-child{font-size:13px;text-transform:uppercase}\n"
    "#score{font-size:28px}\n"
    "#board{background:#bbada0;border-radius:6px;padding:8px;display:grid;grid-template-columns:repeat(4,1fr);gap:8px}\n"
    ".cell{aspect-ratio:1;background:rgba(238,228,218,0.35);border-radius:4px;display:flex;justify-content:center;align-items:center;font-size:40px;font-weight:bold;color:#776e65;transition:all .1s}\n"
    ".tile-2{background:#eee4da}.tile-4{background:#ede0c8}\n"
    ".tile-8{background:#f2b179;color:#f9f6f2}.tile-16{background:#f59563;color:#f9f6f2}\n"
    ".tile-32{background:#f67c5f;color:#f9f6f2}.tile-64{background:#f65e3b;color:#f9f6f2}\n"
    ".tile-128{background:#edcf72;color:#f9f6f2}.tile-256{background:#edcc61;color:#f9f6f2}\n"
    ".tile-512{background:#edc850;color:#f9f6f2}.tile-1024{background:#edc53f;color:#f9f6f2}\n"
    ".tile-2048{background:#edc22e;color:#f9f6f2}.tile-super{background:#3c3a32;color:#f9f6f2}\n"
    "#btns{margin-top:16px;display:flex;gap:8px;flex-wrap:wrap}\n"
    "#btns button{padding:12px 20px;font-size:15px;font-weight:bold;border:none;border-radius:6px;cursor:pointer;background:#8f7a66;color:#fff;flex:1;min-width:80px;transition:background .15s}\n"
    "#btns button:hover{background:#9f8b76}\n"
    "#btns button:active{background:#7a6658}\n"
    "#msg{margin-top:14px;font-size:22px;font-weight:bold;text-align:center;color:#776e65;min-height:30px}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<div id=\"app\">\n"
    "<div id=\"head\"><h1>2048</h1><div id=\"scbox\"><div>Score</div><span id=\"score\">0</span></div></div>\n"
    "<div id=\"board\"></div>\n"
    "<div id=\"btns\">\n"
    "<button onclick=\"newGame()\">New Game</button>\n"
    "<button onclick=\"undo()\">Undo</button>\n"
    "<button onclick=\"saveGame()\">Save</button>\n"
    "<button onclick=\"exitGame()\">Exit</button>\n"
    "</div>\n"
    "<div id=\"msg\"></div>\n"
    "</div>\n"
    "<script>\n"
    "function render(d){var b=document.getElementById('board');b.innerHTML='';\n"
    "for(var i=0;i<4;i++)for(var j=0;j<4;j++){var c=document.createElement('div');c.className='cell';\n"
    "var v=d.board[i][j];if(v>0){c.textContent=v;\n"
    "if(v<=2048)c.classList.add('tile-'+v);else c.classList.add('tile-super');\n"
    "if(v<100)c.style.fontSize='40px';else if(v<1000)c.style.fontSize='32px';\n"
    "else if(v<10000)c.style.fontSize='24px';else c.style.fontSize='18px'}\n"
    "b.appendChild(c)}\n"
    "document.getElementById('score').textContent=d.score;\n"
    "var m=document.getElementById('msg');\n"
    "if(d.gameover)m.textContent='Game Over!';\n"
    "else if(d.won)m.textContent='You Win!';\n"
    "else m.textContent=''}\n"
    "function fetchState(){var x=new XMLHttpRequest();x.open('GET','/state',true);\n"
    "x.onload=function(){render(JSON.parse(x.responseText))};x.send()}\n"
    "function move(d){var x=new XMLHttpRequest();\n"
    "x.open('POST','/move',true);x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');\n"
    "x.onload=function(){if(x.status==200)render(JSON.parse(x.responseText))};x.send('dir='+d)}\n"
    "function undo(){var x=new XMLHttpRequest();x.open('POST','/undo',true);\n"
    "x.onload=function(){if(x.status==200)render(JSON.parse(x.responseText))};x.send()}\n"
    "function saveGame(){var x=new XMLHttpRequest();x.open('POST','/save',true);\n"
    "x.onload=function(){var d=JSON.parse(x.responseText);document.getElementById('msg').textContent=d.message};x.send()}\n"
    "function newGame(){var x=new XMLHttpRequest();x.open('POST','/new',true);\n"
    "x.onload=function(){if(x.status==200)render(JSON.parse(x.responseText))};x.send()}\n"
    "function exitGame(){var x=new XMLHttpRequest();x.open('POST','/exit',true);\n"
    "x.onload=function(){var d=JSON.parse(x.responseText);document.getElementById('msg').textContent=d.message;\n"
    "document.body.innerHTML='<div style=\"text-align:center;margin-top:100px;font-size:36px;color:#776e65\">Goodbye!</div>'};x.send()}\n"
    "document.addEventListener('keydown',function(e){\n"
    "var k=e.key;if(k=='ArrowUp'||k=='w'||k=='W'){e.preventDefault();move('up')}\n"
    "else if(k=='ArrowDown'||k=='s'||k=='S'){e.preventDefault();move('down')}\n"
    "else if(k=='ArrowLeft'||k=='a'||k=='A'){e.preventDefault();move('left')}\n"
    "else if(k=='ArrowRight'||k=='d'||k=='D'){e.preventDefault();move('right')}})\n"
    "fetchState()\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

/* ==================================================================
 *  server init
 * ================================================================*/

static sock_t create_server(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n"); return BAD_SOCK;
    }
#endif

    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (!VALID_SOCK(s)) {
#ifdef _WIN32
        WSACleanup();
#endif
        return BAD_SOCK;
    }

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCK_ERR) {
        printf("Failed to bind port %d (maybe in use)\n", PORT);
        CLOSE_SOCK(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return BAD_SOCK;
    }

    if (listen(s, 5) == SOCK_ERR) {
        printf("listen failed\n");
        CLOSE_SOCK(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return BAD_SOCK;
    }

    /* set accept timeout so we can detect shutdown */
#ifdef _WIN32
    int tv = 1000; /* ms */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    return s;
}

/* ==================================================================
 *  main
 * ================================================================*/

int main(void) {
    srand((unsigned)time(NULL));

    Game state;
    init_game(&state);

    if (load_game(&state))
        printf("Loaded saved game from %s\n", SAVE_FILE);

    g_server = create_server();
    if (!VALID_SOCK(g_server)) {
        printf("Server startup failed\n");
        return 1;
    }

    printf("\n");
    printf("  2048 Game Server\n");
    printf("  ───────────────\n");
    printf("  URL:  http://localhost:%d\n", PORT);
    printf("  Keys: WASD / Arrow keys\n");
    printf("  Exit: Ctrl+C or click Exit button\n");
    printf("\n");

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    while (running) {
        struct sockaddr_in cli;
#ifdef _WIN32
        int len = sizeof(cli);
#else
        socklen_t len = sizeof(cli);
#endif
        sock_t client = accept(g_server, (struct sockaddr*)&cli, &len);
        if (!VALID_SOCK(client)) {
            if (running) {
                /* timeout – just continue */
                continue;
            }
            break;
        }
        handle_client(client, &state);
    }

    if (VALID_SOCK(g_server)) CLOSE_SOCK(g_server);
#ifdef _WIN32
    WSACleanup();
#endif
    printf("Server stopped.\n");
    return 0;
}
