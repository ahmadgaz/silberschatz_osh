#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>

#define MAX_LINE 80   /* The maximum length command */
#define MAX_ARGS (MAX_LINE / 2)
#define READ_END 0
#define WRITE_END 1

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
    int c0 = lex_peek(lx, 0);
    if (c0 < 0 || !fn(c0)) return 0;
    size_t offset = 0;
    for (;;) {
	int next = lex_peek(lx, offset + 1);
	if (next < 0 || !fn(next)) break;
	offset++;
    }
    return offset + 1;
}

static int is_ws (int c) { return (c == ' ') || (c == '\t') || (c == '\r'); }

static void skip_ws (Lexer *lx) {
    for (;;) {
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
    if (!tok.word) { perror("malloc(tok)"); exit(1); }
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

static void free_tok_word (Token *tok) {
    if (tok->kind == T_WORD && tok->word) {
        free(tok->word);
	tok->word = NULL;
    }
}

// --- CMD PARSER ---
typedef enum { R_NONE=0, R_IN, R_OUT } RedirKind;

/* Example Structure:
 * "ls -l | less"
 * Cmd{ argv["less"], ..., pipe_cmd: Cmd{ argv["ls", "-l"], ... }}
 * */
typedef struct Cmd Cmd;
struct Cmd {
    char **argv;
    int argc;
    int is_background;
    int uses_history;
    char *redir_in_path;
    char *redir_out_path;
    Cmd *pipe_cmd;
};

static void cmd_init (Cmd *cmd) {
    cmd->argv = (char **)malloc(sizeof(char *) * (MAX_ARGS + 1));
    if (!cmd->argv) { perror("malloc(argv)"); exit(1); }
    cmd->argc = 0;
    cmd->is_background = 0;
    cmd->uses_history = 0;
    cmd->redir_in_path = NULL;
    cmd->redir_out_path = NULL;
    cmd->pipe_cmd = NULL;
}

static int parse_cmd (Lexer *lx, Cmd *out) {
    Token tok = next_token(lx);

    // "!!" only, no junk after
    if (tok.kind == T_DBANG) {
        Token t2 = next_token(lx);
        if (t2.kind != T_EOF) { free_tok_word(&t2); return -2; }
        out->uses_history = 1;
        return 0;
    }

    for (;;) {
        // collect words
        if (tok.kind == T_WORD) {
            if (out->argc >= MAX_ARGS) { free_tok_word(&tok); return -1; }
            out->argv[out->argc++] = tok.word;
            tok = next_token(lx);
	    continue;
        }
	
	// handle redirs
	if (tok.kind == T_OUT || tok.kind == T_IN) {
            RedirKind rk = (tok.kind==T_OUT) ? R_OUT : R_IN;

	    // sink with pipe can't also take a file input
            if (tok.kind == T_IN && out->pipe_cmd != NULL) return -2;

	    // duplicate redirect
	    if (tok.kind == T_IN && out->redir_in_path) return -2;
	    if (tok.kind == T_OUT && out->redir_out_path) return -2;

            // expect filename
            Token t2 = next_token(lx);
            if (t2.kind != T_WORD){ free_tok_word(&t2); return -2; }
	    if (tok.kind == T_IN) out->redir_in_path = t2.word;
	    if (tok.kind == T_OUT) out->redir_out_path = t2.word;
            tok = next_token(lx);
            continue;
	}

        // handle pipes
        if (tok.kind == T_PIPE) {
	    // lhs needs to exist
	    if (out->argc == 0 && out->redir_in_path == NULL && out->redir_out_path == NULL) return -2;

	    // piped command cannot output to a pipe and an output file
	    if (out->redir_out_path != NULL) return -2;

	    // allocate a new space for the lhs
	    Cmd *prev_cmd = (Cmd *)malloc(sizeof(Cmd));
	    if (!prev_cmd) { perror("malloc(cmd)"); exit(1); }

	    // argv must be null terminated in order to be valid for exec
	    out->argv[out->argc] = NULL;

	    memcpy(prev_cmd, out, sizeof(Cmd));

	    // reinitialize our working command so we can parse into it the rhs
	    cmd_init(out);

	    // point it's child to the lhs
	    out->pipe_cmd = prev_cmd;

	    tok = next_token(lx);
	    continue;
	}

        // '&' at the end means background process
        if (tok.kind == T_AMP) {
            out->is_background = 1;
            tok = next_token(lx);

            // must be end of line
            if (tok.kind != T_EOF) { free_tok_word(&tok); return -2; }
	    break;
        }


	if (tok.kind == T_EOF) break;

	// unsupported ???
	return -2;
    }

    out->argv[out->argc] = NULL;
    return (out->argc == 0 && out->redir_in_path == NULL && out->redir_out_path == NULL) ? 1 : 0;
}

// free string/arrays of a cmd
static void free_cmd_node (Cmd *cmd) {
    for (int i = 0; i < cmd->argc; i++) {
        free(cmd->argv[i]);
        cmd->argv[i] = NULL;
    }
    free(cmd->redir_in_path);
    free(cmd->redir_out_path);
    cmd->redir_in_path = NULL;
    cmd->redir_out_path = NULL;

    free(cmd->argv);
    cmd->argv = NULL;

    cmd->argc = 0;
}

static void free_cmd_chain (Cmd *head) {
    Cmd *node = head;
    while (node) {
        Cmd *next = node->pipe_cmd;
    	free_cmd_node(node);
    	free(node);
	node = next;
    }
}

static void free_cmd (Cmd *head) {
    // free the chain
    if (head->pipe_cmd) {
        free_cmd_chain(head->pipe_cmd);
        head->pipe_cmd = NULL;
    }
    // free the head node
    free_cmd_node(head);
}

static void exec_cmd (Cmd *cmd) {
    // pipe (there exists command we need to pipe input from)
    if (cmd->pipe_cmd != NULL) {
        int fd[2];
	if (pipe(fd) == -1) { puts("Pipe failed."); free_cmd(cmd); exit(1); }
	pid_t pid = fork();
        if (pid < 0) {
            perror("fork(pipe)");
            free_cmd(cmd);
            exit(1);
        } else if (pid == 0) {
            // child
	    if (dup2(fd[WRITE_END], STDOUT_FILENO) < 0) { perror("dup2(pipe_r)"); free_cmd(cmd); exit(1); }
	    close(fd[READ_END]);
	    close(fd[WRITE_END]);
	    exec_cmd(cmd->pipe_cmd);
	    exit(1); // just in case, idk
        } else {
            // parent
	    if (dup2(fd[READ_END], STDIN_FILENO) < 0) { perror("dup2(pipe_r)"); free_cmd(cmd); exit(1); }
	    close(fd[READ_END]);
	    close(fd[WRITE_END]);
	    signal(SIGCHLD, SIG_IGN);
        }
    }

    // redir
    if (cmd->redir_out_path != NULL) {
        int out = open(cmd->redir_out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0){ perror("open(out)"); free_cmd(cmd); exit(1); }
        if (dup2(out, STDOUT_FILENO) < 0) { perror("dup2(out)"); free_cmd(cmd); exit(1); }
        close(out);
    }
    if (cmd->redir_in_path != NULL) {
        int in = open(cmd->redir_in_path, O_RDONLY);
        if (in < 0) { perror("open(in)"); free_cmd(cmd); exit(1); }
        if (dup2(in, STDIN_FILENO) < 0) { perror("dup2(in)"); free_cmd(cmd); exit(1); }
        close(in);
    } 
    
    execvp(cmd->argv[0], cmd->argv);
    
    // if execvp doesn't replace the current (child) process image with the new program, throw an error
    perror("execvp()");
    free_cmd(cmd);
    exit(1);
}

// --- MAIN ---
int main(void) {
    char prev_buf[MAX_LINE] = "";
    char buf[MAX_LINE];

    for (;;) {
	// get input
        printf("osh> ");
        fflush(stdout);
        if (fgets(buf, MAX_LINE, stdin) == NULL) break;

        // strip newline
        buf[strcspn(buf, "\n")] = '\0';

	// empty line
	if (buf[0] == '\0') continue;

        // exit command
        if (strcmp(buf, "exit") == 0) break;

	// parse buffer
	Cmd cmd; cmd_init(&cmd);
	Lexer lx; lex_init(&lx, buf);
	int p_res = parse_cmd(&lx, &cmd);

	if (p_res == 1) { free_cmd(&cmd); continue; } // empty
	if (p_res == -1) { puts("Too many arguments."); free_cmd(&cmd); continue; }
	if (p_res == -2) { puts("Syntax error."); free_cmd(&cmd); continue; }

	// history
	if (cmd.uses_history) {
	    if (prev_buf[0] == '\0') { puts("No commands in history."); free_cmd(&cmd); continue; }
	    puts(prev_buf);

	    // relex and parsse w/ prev line
	    lex_init(&lx, prev_buf);
	    p_res = parse_cmd(&lx, &cmd);
	    if (p_res != 0) { puts("Error parsing history."); free_cmd(&cmd); continue; }
	} else {
            strcpy(prev_buf, buf);
	}

        // empty
        if (cmd.argc == 0) { free_cmd(&cmd); continue; }

        // fork and execute
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork()");
            free_cmd(&cmd);
            exit(1);
        } else if (pid == 0) {
            // child
	    exec_cmd(&cmd);
        } else {
            // parent
            int status;
            if (!cmd.is_background) waitpid(pid, &status, 0);

            // reap foreground children (no zombies)
            while (waitpid(-1, NULL, WNOHANG) > 0) { /* reaped one child */ }
            free_cmd(&cmd);
        }
    }

    // exit message
    puts("Ciao!");
    return 0;
}
