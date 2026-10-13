#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdbool.h>
struct command {
    char *argv[33], *input, *output;
    int argc, append;
};
static int last_status;
static int builtin(struct command *c) {
    if (!c->argc) {
        return 0;
    }
    if (!strcmp(c->argv[0], "exec")) {
        if (c->argc < 2) {
            return 0;
        }
        execve(c->argv[1], c->argv + 1, environ);
        if (errno == ENOENT && !strchr(c->argv[1], '/')) {
            execvp(c->argv[1], c->argv + 1);
        }
        perror(c->argv[1]);
        last_status = 127;
        return 1;
    }
    if (!strcmp(c->argv[0], "exit")) {
        exit(c->argc > 1 ? atoi(c->argv[1]) : last_status);
    }
    if (!strcmp(c->argv[0], "cd")) {
        int r = chdir(c->argc > 1 ? c->argv[1] : "/");
        if (r) {
            perror("cd");
        }
        return r ? 1 : 0;
    }
    if (!strcmp(c->argv[0], "status")) {
        printf("%d\n", last_status);
        return 0;
    }
    return -1;
}
static int redirect(struct command *c) {
    if (c->input) {
        int fd = open(c->input, O_RDONLY);
        if (fd < 0) {
            perror(c->input);
            return -1;
        }
        if (dup2(fd, 0) < 0) {
            close(fd);
            return -1;
        }
        if (fd != 0) {
            close(fd);
        }
    }
    if (c->output) {
        int fd = open(c->output, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0666);
        if (fd < 0) {
            perror(c->output);
            return -1;
        }
        if (dup2(fd, 1) < 0) {
            close(fd);
            return -1;
        }
        if (fd != 1) {
            close(fd);
        }
    }
    return 0;
}
static int execute(struct command *commands, int count, bool background) {
    if (count == 1 && !background && !commands[0].input && !commands[0].output) {
        int r = builtin(commands);
        if (r >= 0) {
            return r;
        }
    }
    int previous = -1, status = 0, started = 0;
    pid_t children[8];
    for (int i = 0; i < count; i++) {
        int fds[2] = {-1, -1};
        if (i + 1 < count && pipe(fds)) {
            perror("pipe");
            break;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (fds[0] >= 0) {
                close(fds[0]);
                close(fds[1]);
            }
            break;
        }
        if (!pid) {
            if (previous >= 0) {
                dup2(previous, 0);
                close(previous);
            }
            if (fds[1] >= 0) {
                dup2(fds[1], 1);
                close(fds[0]);
                close(fds[1]);
            }
            if (redirect(&commands[i])) {
                _exit(1);
            }
            int r = builtin(&commands[i]);
            if (r >= 0) {
                _exit(r);
            }
            execvp(commands[i].argv[0], commands[i].argv);
            perror(commands[i].argv[0]);
            _exit(127);
        }
        children[started++] = pid;
        if (previous >= 0) {
            close(previous);
        }
        if (fds[1] >= 0) {
            close(fds[1]);
        }
        previous = fds[0];
    }
    if (previous >= 0) {
        close(previous);
    }
    if (background) {
        if (started) {
            printf("[%d]\n", children[started - 1]);
        }
        return 0;
    }
    for (int i = 0; i < started; i++) {
        if (waitpid(children[i], &status, 0) < 0) {
            perror("wait");
        }
    }
    return WEXITSTATUS(status);
}
static int parse(char *line) {
    char storage[2048];
    size_t used = 0;
    char *tokens[128];
    int operators[128], ntokens = 0;
    char *p = line;
    while (*p && ntokens < 128) {
        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (!*p || *p == '#') {
            p += strlen(p);
            break;
        }
        operators[ntokens] = 0;
        tokens[ntokens] = storage + used;
        if (strchr("|;&<>", *p)) {
            int op = *p++;
            if (op == '>' && *p == '>') {
                op = 256;
                p++;
            }
            operators[ntokens++] = op;
            continue;
        }
        char quote = 0;
        while (*p && (quote || !strchr(" \t\n|;&<>", *p))) {
            if ((*p == '\'' || *p == '"') && (!quote || quote == *p)) {
                quote = quote ? 0 : *p;
                p++;
                continue;
            }
            if (*p == '\\' && quote != '\'' && p[1]) {
                p++;
            } else if (*p == '$' && quote != '\'') {
                p++;
                char name[64];
                size_t n = 0;
                if (*p == '?') {
                    char value[16];
                    snprintf(value, sizeof(value), "%d", last_status);
                    size_t len = strlen(value);
                    if (used + len >= sizeof(storage)) {
                        return -1;
                    }
                    memcpy(storage + used, value, len);
                    used += len;
                    p++;
                    continue;
                }
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_') {
                    if (n + 1 < sizeof(name)) {
                        name[n++] = *p;
                    }
                    p++;
                }
                name[n] = 0;
                char *value = getenv(name);
                if (value) {
                    size_t len = strlen(value);
                    if (used + len >= sizeof(storage)) {
                        return -1;
                    }
                    memcpy(storage + used, value, len);
                    used += len;
                }
                continue;
            }
            if (used + 1 >= sizeof(storage)) {
                return -1;
            }
            storage[used++] = *p++;
        }
        if (quote || used >= sizeof(storage)) {
            return -1;
        }
        storage[used++] = 0;
        ntokens++;
    }
    if (*p) {
        return -1;
    }
    struct command commands[8] = {0};
    int count = 1;
    for (int i = 0; i <= ntokens; i++) {
        int op = i == ntokens ? ';' : operators[i];
        struct command *c = &commands[count - 1];
        if (op == ';' || op == '&') {
            if (c->argc) {
                last_status = execute(commands, count, op == '&');
            } else if (count > 1) {
                return -1;
            }
            memset(commands, 0, sizeof(commands));
            count = 1;
        } else if (op == '|') {
            if (!c->argc || count == 8) {
                return -1;
            }
            count++;
        } else if (op == '<' || op == '>' || op == 256) {
            if (++i >= ntokens || operators[i]) {
                return -1;
            }
            if (op == '<') {
                c->input = tokens[i];
            } else {
                c->output = tokens[i];
                c->append = op == 256;
            }
        } else {
            if (c->argc == 32) {
                return -1;
            }
            c->argv[c->argc++] = tokens[i];
        }
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "-c")) {
        return parse(argv[2]) ? 2 : last_status;
    }
    int script = argc > 1 ? open(argv[1], O_RDONLY) : 0;
    if (script < 0) {
        perror(argv[1]);
        return 1;
    }
    for (;;) {
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0)
            ;
        if (!script) {
            printf("kaly:%u$ ", getuid());
        }
        char line[1024];
        size_t n = 0;
        for (;;) {
            char c;
            ssize_t r = read(script, &c, 1);
            if (r <= 0) {
                return last_status;
            }
            if (c == '\r') {
                c = '\n';
            }
            if (c == '\b' || c == 127) {
                if (n) {
                    n--;
                    if (!script) {
                        write(1, "\b \b", 3);
                    }
                }
                continue;
            }
            if (!script) {
                write(1, &c, 1);
            }
            if (c == '\n') {
                break;
            }
            if (n + 1 < sizeof(line)) {
                line[n++] = c;
            }
        }
        line[n] = 0;
        if (parse(line)) {
            fputs("syntax error\n", stderr);
            last_status = 2;
        }
    }
}
