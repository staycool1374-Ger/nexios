#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>

#define MAX_ARGS 16
#define MAX_CMDS 8
#define LINE_BUF 256

/// @brief Environment store bounds (issue #310): every entry sits far
/// inside the kernel exec window (64 args x 256 B, MAX_EXEC_ARGS /
/// MAX_EXEC_ARG_LEN) — overlong exports are rejected at set time.
#define MAX_ENV 16
#define ENV_VAL_LEN 128
/// @brief Resolver bounds: max PATH dirs scanned, max chars per segment,
/// and the exec-path buffer (matches LINE_BUF so any typed line fits).
#define MAX_PATH_DIRS 16
#define MAX_DIR_SEG 64
#define EXEC_PATH_LEN 256

static char line[LINE_BUF];
static int last_status = 0;

static char env_buf[MAX_ENV][ENV_VAL_LEN];
static char *env_ptrs[MAX_ENV + 1];
char **environ;

static char exec_path[EXEC_PATH_LEN];

/// @brief Length of a NUL-terminated string (freestanding, no strlen).
static int sh_strlen(const char *s) {
    int n = 0;
    while (s[n])
        ++n;
    return n;
}

/// @brief True when `s` starts with `prefix` and the next char is '='.
static int sh_name_matches(const char *s, const char *prefix) {
    int i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i])
            return 0;
        ++i;
    }
    return s[i] == '=';
}

/// @brief Look up a variable in the static store (NULL when absent).
static const char *sh_getenv(const char *name) {
    if (!name || !name[0])
        return 0;
    for (int i = 0; env_ptrs[i]; ++i) {
        if (sh_name_matches(env_ptrs[i], name))
            return env_ptrs[i] + sh_strlen(name) + 1;
    }
    return 0;
}

/// @brief Set a variable (update in place or append). Fail-closed: -1
/// when the entry does not fit or the store is full; the store is
/// never left half-written.
static int sh_setenv(const char *name, const char *value) {
    if (!name || !name[0] || !value)
        return -1;
    int namelen = sh_strlen(name);
    int vallen = sh_strlen(value);
    if (namelen + 1 + vallen + 1 > ENV_VAL_LEN)
        return -1;
    for (int i = 0; env_ptrs[i]; ++i) {
        if (sh_name_matches(env_ptrs[i], name)) {
            int k = 0;
            for (int j = 0; j < namelen; ++j)
                env_buf[i][k++] = name[j];
            env_buf[i][k++] = '=';
            for (int j = 0; j <= vallen; ++j)
                env_buf[i][k++] = value[j];
            return 0;
        }
    }
    int n = 0;
    while (env_ptrs[n])
        ++n;
    if (n >= MAX_ENV)
        return -1;
    int k = 0;
    for (int j = 0; j < namelen; ++j)
        env_buf[n][k++] = name[j];
    env_buf[n][k++] = '=';
    for (int j = 0; j <= vallen; ++j)
        env_buf[n][k++] = value[j];
    env_ptrs[n] = env_buf[n];
    env_ptrs[n + 1] = 0;
    return 0;
}

/// @brief Install the default environment (PATH=/bin:/).
/// NOTE (issue #310): sh never imports a passed envp. The runelf load
/// path builds an empty env, and deriving envp from rsp in crt0 faults
/// there (verified: 3-arg main + import = instant #GP; 2-arg = clean
/// boot). Children of sh still receive this store via the exec arg2.
static void sh_env_init(void) {
    env_ptrs[0] = 0;
    environ = env_ptrs;
    sh_setenv("PATH", "/bin:/");
}

static void print_prompt(void) {
    printf("sh$ ");
}

static int read_line(void) {
    int i = 0;
    char c;
    while (i < LINE_BUF - 1) {
        if (read(0, &c, 1) != 1) return -1;
        if (c == '\n') break;
        if (c == '\r') break;
        if (c == '\b' || c == 127) {
            if (i > 0) { --i; write(1, "\b \b", 3); }
            continue;
        }
        write(1, &c, 1);
        line[i++] = c;
    }
    line[i] = '\0';
    write(1, "\n", 1);
    return i;
}

struct Command {
    char* argv[MAX_ARGS + 1];
    int argc;
    const char* redirect_in;
    const char* redirect_out;
};

static int parse_line(struct Command cmds[], int* ncmds) {    int i = 0;
    while (line[i] == ' ') ++i;
    if (line[i] == '\0') return -1;

    char* pipeline[MAX_CMDS];
    int np = 0;
    pipeline[np++] = &line[i];
    for (int j = i; line[j] && np < MAX_CMDS; ++j) {
        if (line[j] == '|') {
            line[j] = '\0';
            pipeline[np++] = &line[j + 1];
        }
    }

    for (int c = 0; c < np; ++c) {
        struct Command* cmd = &cmds[c];
        cmd->argc = 0;
        cmd->redirect_in = 0;
        cmd->redirect_out = 0;

        char* p = pipeline[c];
        while (*p == ' ') ++p;

        while (*p && cmd->argc < MAX_ARGS) {
            if (*p == '<') {
                ++p;
                while (*p == ' ') ++p;
                cmd->redirect_in = p;
                while (*p && *p != ' ') ++p;
                if (*p) { *p++ = '\0'; }
                continue;
            }
            if (*p == '>') {
                ++p;
                while (*p == ' ') ++p;
                cmd->redirect_out = p;
                while (*p && *p != ' ') ++p;
                if (*p) { *p++ = '\0'; }
                continue;
            }
            cmd->argv[cmd->argc++] = p;
            while (*p && *p != ' ') ++p;
            if (*p) { *p++ = '\0'; }
            while (*p == ' ') ++p;
        }
        cmd->argv[cmd->argc] = 0;
    }

    *ncmds = np;
    return np;
}

/// @brief Resolve a command name via PATH into `out` (issue #310).
/// Names containing '/' are copied verbatim (bounded); bare names try
/// each colon-separated PATH dir (`dir + "/" + name`, empty dir means
/// root), probing with open()+close. Fail-closed: -1 when nothing fits
/// or resolves — the caller prints `not found` and exits 127.
static int resolve_path(const char *cmd, char *out, int out_len) {
    if (!cmd || !cmd[0] || !out || out_len <= 0)
        return -1;
    int cmdlen = sh_strlen(cmd);
    int hasslash = 0;
    for (int i = 0; i < cmdlen; ++i) {
        if (cmd[i] == '/') {
            hasslash = 1;
            break;
        }
    }
    if (hasslash) {
        if (cmdlen + 1 > out_len)
            return -1;
        for (int i = 0; i <= cmdlen; ++i)
            out[i] = cmd[i];
        return 0;
    }
    const char *path = sh_getenv("PATH");
    if (!path || !path[0])
        path = "/bin:/";
    int pos = 0;
    for (int dir = 0; dir < MAX_PATH_DIRS; ++dir) {
        int segstart = pos;
        while (path[pos] && path[pos] != ':')
            ++pos;
        int seglen = pos - segstart;
        int last = (path[pos] == '\0');
        if (path[pos] == ':')
            ++pos;
        if (seglen > MAX_DIR_SEG) {
            if (last)
                break;
            continue;
        }
        if (seglen + 1 + cmdlen + 1 > out_len) {
            if (last)
                break;
            continue;
        }
        int k = 0;
        for (int i = 0; i < seglen; ++i)
            out[k++] = path[segstart + i];
        out[k++] = '/';
        for (int i = 0; i <= cmdlen; ++i)
            out[k++] = cmd[i];
        int fd = open(out, 0);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        if (last)
            break;
    }
    return -1;
}

static int run_command(struct Command* cmd, int input_fd, int output_fd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) return (int)pid;

    if (input_fd != -1) {
        dup2(input_fd, 0);
        close(input_fd);
    }
    if (output_fd != -1) {
        dup2(output_fd, 1);
        close(output_fd);
    }

    if (cmd->redirect_in) {
        int fd = open(cmd->redirect_in, 0);
        if (fd >= 0) { dup2(fd, 0); close(fd); }
    }
    if (cmd->redirect_out) {
        int fd = open(cmd->redirect_out, 1);
        if (fd >= 0) { dup2(fd, 1); close(fd); }
    }

    if (strcmp(cmd->argv[0], "cd") == 0) {
        const char* target = cmd->argv[1] ? cmd->argv[1] : "/";
        if (chdir(target) < 0)
            printf("cd: %s: no such directory\n", target);
        _exit(0);
    }

    if (strcmp(cmd->argv[0], "export") == 0) {
        // Print-only fallback for pipelines: real mutation happens in
        // the parent builtin (state set in a forked child would die
        // with it).
        if (cmd->argv[1]) {
            const char *v = sh_getenv(cmd->argv[1]);
            if (v)
                printf("export %s=%s\n", cmd->argv[1], v);
            else
                printf("export %s\n", cmd->argv[1]);
        }
        _exit(0);
    }

    if (strcmp(cmd->argv[0], "exit") == 0) {
        _exit(0);
    }

    if (resolve_path(cmd->argv[0], exec_path, EXEC_PATH_LEN) < 0) {
        printf("sh: %s: not found\n", cmd->argv[0]);
        _exit(127);
        return -1;
    }
    __syscall5(20, (long)exec_path, (long)cmd->argv, (long)environ, 0);
    printf("sh: %s: not found\n", cmd->argv[0]);
    _exit(127);
    return -1;
}

static int execute_pipeline(struct Command cmds[], int ncmds) {
    int prev_fd = -1;
    int first_pid = -1;

    for (int i = 0; i < ncmds; ++i) {
        int pipe_fd[2] = {-1, -1};
        int out_fd = -1;

        if (i < ncmds - 1) {
            if (pipe(pipe_fd) < 0) return -1;
            out_fd = pipe_fd[1];
        }

        int pid = run_command(&cmds[i], prev_fd, out_fd);
        if (i == 0) first_pid = pid;

        if (prev_fd != -1) close(prev_fd);
        if (out_fd != -1) close(out_fd);

        prev_fd = pipe_fd[0];
    }

    int status = 0;
    for (int i = 0; i < ncmds; ++i) {
        int wstatus;
        pid_t r = waitpid(-1, &wstatus, 0);
        if (r > 0) status = wstatus;
    }
    return status;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    sh_env_init();

    printf("Jarvis RTOS Userspace Shell\n");

    while (1) {
        print_prompt();
        if (read_line() < 0) break;

        struct Command cmds[MAX_CMDS];
        int ncmds;
        if (parse_line(cmds, &ncmds) <= 0) continue;

        if (cmds[0].argc == 0) continue;

        if (strcmp(cmds[0].argv[0], "cd") == 0 && ncmds == 1) {
            const char* target = cmds[0].argv[1] ? cmds[0].argv[1] : "/";
            if (chdir(target) < 0)
                printf("cd: %s: no such directory\n", target);
            continue;
        }

        if (strcmp(cmds[0].argv[0], "export") == 0 && ncmds == 1) {
            // Parent-side builtin: mutation must happen here, not in a
            // forked child (child state dies with it — the old stub bug).
            const char *arg = cmds[0].argv[1];
            if (!arg) {
                for (int i = 0; env_ptrs[i]; ++i)
                    printf("export %s\n", env_ptrs[i]);
            } else if (cmds[0].argv[2]) {
                printf("export: usage: export [NAME[=VALUE]]\n");
            } else {
                int k = 0;
                while (arg[k] && arg[k] != '=')
                    ++k;
                if (k == 0) {
                    printf("export: usage: export [NAME[=VALUE]]\n");
                } else if (!arg[k]) {
                    const char *v = sh_getenv(arg);
                    if (v)
                        printf("export %s=%s\n", arg, v);
                } else {
                    char name[ENV_VAL_LEN];
                    if (k >= ENV_VAL_LEN) {
                        printf("export: name too long\n");
                    } else {
                        for (int j = 0; j < k; ++j)
                            name[j] = arg[j];
                        name[k] = '\0';
                        if (sh_setenv(name, arg + k + 1) < 0)
                            printf("export: failed\n");
                    }
                }
            }
            continue;
        }

        if (strcmp(cmds[0].argv[0], "exit") == 0) break;

        last_status = execute_pipeline(cmds, ncmds);
    }

    printf("sh: exiting\n");
    return 0;
}