#include "suggest.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "paths.h"
#include "util.h"

char *fuzzy_suggest(const char *query, const char *const *candidates, size_t count) {
    if (count == 0) {
        return NULL;
    }
    char *fzf = path_search("fzf", NULL, NULL);
    if (!fzf) {
        return NULL;
    }

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        free(fzf);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(fzf);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl(fzf, fzf, "--filter", query, (char *)NULL);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    /* The candidate lists here are tiny (a handful of shim/command names),
     * so writing them all up front and only then reading the result is
     * safe -- no risk of filling the pipe buffer and deadlocking against a
     * child that hasn't started producing output yet. */
    for (size_t i = 0; i < count; i++) {
        dprintf(in_pipe[1], "%s\n", candidates[i]);
    }
    close(in_pipe[1]);

    DynBuf out;
    dynbuf_init(&out);
    char chunk[256];
    ssize_t n;
    while ((n = read(out_pipe[0], chunk, sizeof(chunk))) > 0) {
        dynbuf_append(&out, chunk, (size_t)n);
    }
    close(out_pipe[0]);
    waitpid(pid, NULL, 0);
    free(fzf);

    const char *text = dynbuf_cstr(&out);
    const char *newline = strchr(text, '\n');
    size_t line_len = newline ? (size_t)(newline - text) : strlen(text);
    if (line_len == 0) {
        dynbuf_free(&out);
        return NULL;
    }
    char *result = xmalloc(line_len + 1);
    memcpy(result, text, line_len);
    result[line_len] = '\0';
    dynbuf_free(&out);
    return result;
}
