#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <ctype.h>

#define MAX_LINE 80   /* The maximum length command */
#define MAX_ARGS (MAX_LINE / 2)

// --- TOKEN ---
typedef enum {
    T_EOF = 0,	// \0
    T_AMP,	// &
    T_DBANG,	// !!
    T_OUT,	// >
    T_IN,	// <
    T_PIPE,	// |
    T_WORD	// words
} TokKind;

typedef struct { size_t start, size; } Span;

typedef struct {
    TokKind kind;
    Span pos;
    char *word;
} Token;

static void tok_init (Token *tok, TokKind kind, size_t start, size_t size, char *word) {
    tok->kind = kind;
    tok->pos.start = start;
    tok->pos.size = size;
    tok->word = word;
}

// --- LEXER ---
typedef struct {
    const char *content;
    size_t len;
    size_t pos;
} Lexer;

static void lex_init (Lexer *lx, const char *content) {
    lx->content = content;
    lx->len = strlen(content);
    lx->pos = 0;
}

static int lex_peek (const Lexer *lx, size_t offset) {
    size_t idx = lx->pos + offset;
    if (idx >= lx->len) return -1;
    return (unsigned char)lx->content[idx];
}

static void lex_advance (Lexer *lx, size_t n) {
    lx->pos += n;
    if (lx->pos > lx->len) lx->pos = lx->len;
}

static Token make_n_char_token (Lexer *lx, TokKind k, size_t n) {
    Token tok; tok_init(&tok, k, lx->pos, n, NULL);
    lex_advance(lx, n);
    return tok;
}

typedef int (*pred_fn)(int ch);
static size_t read_span_len (const Lexer *lx, pred_fn fn) {
    size_t offset = 0;
    if (lex_peek(lx, 0) < 0) return 0;
    while (1) {
	int next = lex_peek(lx, offset + 1);
	if (next < 0 || !fn(next)) break;
	offset++;
    }
    return offset + 1;
}

static int is_ws (int c) { return (c == ' ') || (c == '\t') || (c == '\r'); }

static void skip_ws (Lexer *lx) {
    for(;;) {
        int c = lex_peek(lx, 0);
	if (is_ws(c)) {
            lex_advance(lx, 1);
	} else {
	    break;
	}
    }
}

static int is_word (int c) { return !(is_ws(c) || c == '&' || c == '>' || c == '<' || c == '|'); }

static Token make_word_token (Lexer *lx) {
    size_t len = read_span_len(lx, is_word);
    const char *s = lx->content + lx->pos;
    Token tok = make_n_char_token(lx, T_WORD, len);
    tok.word = (char *)malloc(len + 1);
    if (!tok.word) {
        perror("malloc()");
        exit(1);
    }
    memcpy(tok.word, s, len);
    tok.word[len] = '\0';
    return tok;
}

static Token next_token (Lexer *lx) {
    skip_ws(lx);
    int c = lex_peek(lx, 0);
    if (c < 0) return make_n_char_token(lx, T_EOF, 1);

    switch (c) {
        case '\n': return make_n_char_token(lx, T_EOF, 1); // treat as EOF for now (will be stripped at fgets)
	case '&': return make_n_char_token(lx, T_AMP, 1);
	case '!': {
	    int next = lex_peek(lx, 1);
	    if (next == '!') return make_n_char_token(lx, T_DBANG, 2);
	    return make_word_token(lx);
	}
	case '>': return make_n_char_token(lx, T_OUT, 1);
	case '<': return make_n_char_token(lx, T_IN, 1);
	case '|': return make_n_char_token(lx, T_PIPE, 1);
	default: return make_word_token(lx);
    }
}

static void free_token_word (Token *tok) {
    if (tok->kind == T_WORD && tok->word) {
        free(tok->word);
	tok->word = NULL;
    }
}

// --- PARSER ---
typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc;
    int is_background;
    int uses_history;
} Cmd;

static int parse_command(Lexer *lx, Cmd *out) {
    out->argc = 0;
    out->is_background = 0;
    out->uses_history = 0;

    Token tok = next_token(lx);

    // "!!" only, no junk after
    if (tok.kind == T_DBANG) {
        Token t2 = next_token(lx);
	if (t2.kind != T_EOF) { free_token_word(&t2); return -2; }
	out->uses_history = 1;
	return 0;
    }

    // collect words
    while (tok.kind == T_WORD) {
        if (out->argc >= MAX_ARGS) { free_token_word(&tok); return -1; }
	out->argv[out->argc++] = tok.word;
	tok = next_token(lx);
    }

    // '&' at the end means background process
    if (tok.kind == T_AMP) {
        out->is_background = 1;
	tok = next_token(lx);
    }

    // TODO: handle |, <, and >

    // end of line
    if (tok.kind != T_EOF) { free_token_word(&tok); return -2; }

    out->argv[out->argc] = NULL;
    return (out->argc == 0) ? 1 : 0;
}

static void free_cmd_args (Cmd *cmd) {
    for (int i = 0; i < cmd->argc; i++) {
        free(cmd->argv[i]);
	cmd->argv[i] = NULL;
    }
    cmd->argc = 0;
}

// --- MAIN ---
int main(void) {
    char prev_buf[MAX_LINE] = "";
    char buf[MAX_LINE];

    while (1) {
	// get input
        printf("osh> ");
        fflush(stdout);
        if (fgets(buf, MAX_LINE, stdin) == NULL) break;

        // strip newline
	size_t len = strlen(buf);
	size_t nl_pos = strcspn(buf, "\n");
        if (nl_pos < len) buf[nl_pos] = '\0';

	// empty line
	if (buf[0] == '\0') continue;

        // exit command
        if (strcmp(buf, "exit") == 0) break;

	// parse buffer
	Cmd cmd;
	Lexer lx; lex_init(&lx, buf);
	int p_res = parse_command(&lx, &cmd);

	if (p_res == 1) { free_cmd_args(&cmd); continue; } // empty
	if (p_res == -1) { puts("Too many arguments."); free_cmd_args(&cmd); continue; }
	if (p_res == -2) { puts("Syntax error."); free_cmd_args(&cmd); continue; }

	// history
	if (cmd.uses_history) {
	    if (prev_buf[0] == '\0') { puts("No commands in history."); free_cmd_args(&cmd); continue; }
	    puts(prev_buf);

	    // relex and parsse w/ prev line
	    lex_init(&lx, prev_buf);
	    p_res = parse_command(&lx, &cmd);
	    if (p_res != 0) { puts("Error parsing history."); free_cmd_args(&cmd); continue; }
	} else {
            strcpy(prev_buf, buf);
	}

        // empty
        if (cmd.argc == 0) { free_cmd_args(&cmd); continue; }

        // fork and execute
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork()");
	    free_cmd_args(&cmd);
            continue;
        } else if (pid == 0) {
            // child
            execvp(cmd.argv[0], cmd.argv);

	    // if execvp doesn't replace the current (child) process image with the new program, throw an error
            perror("execvp()");
            free_cmd_args(&cmd);
            exit(1);
        } else {
            // parent
            int status;
            if (!cmd.is_background) waitpid(pid, &status, 0);

	    // reap foreground children (no zombies)
	    while (waitpid(-1, NULL, WNOHANG) > 0) { /* reaped one child */ }
            free_cmd_args(&cmd);
        }
    }

    // exit message
    puts("Ciao!");
    return 0;
}
