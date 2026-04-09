/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 4096
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int log_pipe_fd; 
    int stop_requested;
    int run_client_fd; // If positive, we must send a response when it exits
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static int sigchld_pipe[2] = {-1, -1};
static int sigterm_pipe[2] = {-1, -1};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes) {
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') return -1;
    if (mib > ULONG_MAX / (1UL << 20)) return -1;
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index) {
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;
        if (i + 1 >= argc) return -1;
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' || nice_value < -20 || nice_value > 19) return -1;
            req->nice_value = (int)nice_value;
            continue;
        }
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) return -1;
    return 0;
}

static const char *state_to_string(container_state_t state) {
    switch (state) {
        case CONTAINER_STARTING: return "starting";
        case CONTAINER_RUNNING: return "running";
        case CONTAINER_STOPPED: return "stopped";
        case CONTAINER_KILLED: return "killed";
        case CONTAINER_EXITED: return "exited";
        default: return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer) {
    memset(buffer, 0, sizeof(*buffer));
    if (pthread_mutex_init(&buffer->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&buffer->not_empty, NULL) != 0) return -1;
    if (pthread_cond_init(&buffer->not_full, NULL) != 0) return -1;
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer) {
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer) {
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

void *logging_thread(void *arg) {
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    mkdir(LOG_DIR, 0755);
    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0) break;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.length, f);
            fclose(f);
        }
    }
    return NULL;
}

int child_fn(void *arg) {
    child_config_t *cfg = (child_config_t *)arg;

    close(STDIN_FILENO);
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    sethostname(cfg->id, strlen(cfg->id));

    if (mount(cfg->rootfs, cfg->rootfs, "bind", MS_BIND | MS_REC, "") < 0) return 1;
    if (chroot(cfg->rootfs) < 0) return 1;
    if (chdir("/") < 0) return 1;

    mount("proc", "/proc", "proc", 0, NULL);
    
    if (cfg->nice_value != 0) {
        nice(cfg->nice_value);
    }

    char *cmd_args[] = {"/bin/sh", "-c", cfg->command, NULL};
    execvp(cmd_args[0], cmd_args);
    return 1;
}

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid, unsigned long soft_limit_bytes, unsigned long hard_limit_bytes) {
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (monitor_fd >= 0) ioctl(monitor_fd, MONITOR_REGISTER, &req);
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid) {
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (monitor_fd >= 0) ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
    return 0;
}

typedef struct {
    supervisor_ctx_t *ctx;
    int log_read_fd;
    char container_id[CONTAINER_ID_LEN];
} producer_args_t;

void *producer_thread(void *arg) {
    producer_args_t *pargs = (producer_args_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;
    while ((n = read(pargs->log_read_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pargs->container_id, CONTAINER_ID_LEN - 1);
        item.length = n;
        memcpy(item.data, buf, n);
        if (bounded_buffer_push(&pargs->ctx->log_buffer, &item) < 0) break;
    }
    close(pargs->log_read_fd);
    free(pargs);
    return NULL;
}

static void sigchld_handler(int sig) {
    int saved_errno = errno;
    char c = 'C';
    write(sigchld_pipe[1], &c, 1);
    errno = saved_errno;
}

static void sigterm_handler(int sig) {
    int saved_errno = errno;
    char c = 'T';
    write(sigterm_pipe[1], &c, 1);
    errno = saved_errno;
}

static void handle_client(supervisor_ctx_t *ctx, int client_fd) {
    control_request_t req;
    if (read(client_fd, &req, sizeof(req)) <= 0) {
        close(client_fd);
        return;
    }

    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (req.kind == CMD_PS) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *p = ctx->containers;
        char buf[4000] = "ID\tPID\tSTATE\tEXIT\n";
        int len = strlen(buf);
        while (p) {
            len += snprintf(buf + len, sizeof(buf) - len, "%s\t%d\t%s\t%d\n", p->id, p->host_pid, state_to_string(p->state), p->exit_code);
            p = p->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        strncpy(resp.message, buf, sizeof(resp.message)-1);
        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
        return;
    }

    if (req.kind == CMD_LOGS) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[4000] = {0};
            fread(buf, 1, sizeof(buf)-1, f);
            fclose(f);
            resp.status = 0;
            strncpy(resp.message, buf, sizeof(resp.message)-1);
        } else {
            resp.status = 1;
            strcpy(resp.message, "No logs");
        }
        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
        return;
    }

    if (req.kind == CMD_STOP) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *p = ctx->containers;
        while (p) {
            if (strcmp(p->id, req.container_id) == 0 && (p->state == CONTAINER_STARTING || p->state == CONTAINER_RUNNING)) {
                p->stop_requested = 1;
                kill(p->host_pid, SIGTERM);
                break;
            }
            p = p->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        strcpy(resp.message, "Stop signal sent");
        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
        return;
    }

    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        // check unique rootfs
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *chk = ctx->containers;
        while(chk) {
            if (strcmp(chk->id, req.container_id) == 0 && (chk->state == CONTAINER_RUNNING || chk->state == CONTAINER_STARTING)) {
                resp.status = 1;
                strcpy(resp.message, "Container ID in use");
                pthread_mutex_unlock(&ctx->metadata_lock);
                write(client_fd, &resp, sizeof(resp));
                close(client_fd);
                return;
            }
            chk = chk->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        int log_pipes[2];
        pipe(log_pipes);

        child_config_t *cfg = malloc(sizeof(child_config_t));
        memset(cfg, 0, sizeof(*cfg));
        strncpy(cfg->id, req.container_id, CONTAINER_ID_LEN - 1);
        strncpy(cfg->rootfs, req.rootfs, PATH_MAX - 1);
        strncpy(cfg->command, req.command, CHILD_COMMAND_LEN - 1);
        cfg->nice_value = req.nice_value;
        cfg->log_write_fd = log_pipes[1];

        void *stack = malloc(STACK_SIZE);
        pid_t pid = clone(child_fn, (char *)stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, cfg);
        
        close(log_pipes[1]); // Close write end in parent
        
        if (pid < 0) {
            resp.status = 1;
            strcpy(resp.message, "Clone failed");
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
            free(stack);
            free(cfg);
            return;
        }

        container_record_t *rec = malloc(sizeof(container_record_t));
        memset(rec, 0, sizeof(*rec));
        strncpy(rec->id, req.container_id, CONTAINER_ID_LEN - 1);
        rec->host_pid = pid;
        rec->started_at = time(NULL);
        rec->state = CONTAINER_RUNNING;
        rec->soft_limit_bytes = req.soft_limit_bytes;
        rec->hard_limit_bytes = req.hard_limit_bytes;
        rec->log_pipe_fd = log_pipes[0];
        rec->run_client_fd = (req.kind == CMD_RUN) ? client_fd : -1;

        pthread_mutex_lock(&ctx->metadata_lock);
        rec->next = ctx->containers;
        ctx->containers = rec;
        pthread_mutex_unlock(&ctx->metadata_lock);

        register_with_monitor(ctx->monitor_fd, rec->id, rec->host_pid, rec->soft_limit_bytes, rec->hard_limit_bytes);

        producer_args_t *pargs = malloc(sizeof(producer_args_t));
        pargs->ctx = ctx;
        pargs->log_read_fd = log_pipes[0];
        strncpy(pargs->container_id, rec->id, CONTAINER_ID_LEN - 1);

        pthread_t pt;
        pthread_create(&pt, NULL, producer_thread, pargs);
        pthread_detach(pt);

        if (req.kind == CMD_START) {
            resp.status = 0;
            strcpy(resp.message, "Started");
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
        }
        // CMD_RUN will wait until exit in reap_children
    }
}

static void reap_children(supervisor_ctx_t *ctx) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *p = ctx->containers;
        while (p) {
            if (p->host_pid == pid) {
                if (WIFEXITED(status)) {
                    p->exit_code = WEXITSTATUS(status);
                    p->state = p->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    p->exit_signal = WTERMSIG(status);
                    p->exit_code = 128 + WTERMSIG(status);
                    p->state = p->stop_requested ? CONTAINER_STOPPED : (WTERMSIG(status) == SIGKILL ? CONTAINER_KILLED : CONTAINER_EXITED);
                }
                unregister_from_monitor(ctx->monitor_fd, p->id, p->host_pid);
                
                if (p->run_client_fd > 0) {
                    control_response_t resp;
                    memset(&resp, 0, sizeof(resp));
                    resp.status = p->exit_code;
                    snprintf(resp.message, sizeof(resp.message), "Exited with %d", p->exit_code);
                    write(p->run_client_fd, &resp, sizeof(resp));
                    close(p->run_client_fd);
                    p->run_client_fd = -1;
                }
                break;
            }
            p = p->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static int run_supervisor(const char *rootfs) {
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);

    unlink(CONTROL_PATH);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(sfd, 10);
    ctx.server_fd = sfd;

    pipe(sigchld_pipe);
    pipe(sigterm_pipe);

    struct sigaction sa_chld;
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term;
    sa_term.sa_handler = sigterm_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGINT, &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    while (!ctx.should_stop) {
        struct pollfd fds[3];
        fds[0].fd = sfd; fds[0].events = POLLIN;
        fds[1].fd = sigchld_pipe[0]; fds[1].events = POLLIN;
        fds[2].fd = sigterm_pipe[0]; fds[2].events = POLLIN;

        if (poll(fds, 3, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[1].revents & POLLIN) {
            char c; read(fds[1].fd, &c, 1);
            reap_children(&ctx);
        }

        if (fds[2].revents & POLLIN) {
            char c; read(fds[2].fd, &c, 1);
            ctx.should_stop = 1;
        }

        if (fds[0].revents & POLLIN) {
            int cfd = accept(sfd, NULL, NULL);
            if (cfd >= 0) handle_client(&ctx, cfd);
        }
    }

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *p = ctx.containers;
    while (p) {
        if (p->state == CONTAINER_STARTING || p->state == CONTAINER_RUNNING) {
            kill(p->host_pid, SIGTERM);
        }
        p = p->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    close(sfd);
    unlink(CONTROL_PATH);
    if(ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    return 0;
}

static int send_control_request(const control_request_t *req, control_response_t *resp) {
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sfd);
        return -1;
    }

    if (write(sfd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(sfd);
        return -1;
    }

    int n = read(sfd, resp, sizeof(*resp));
    if (n <= 0) {
        close(sfd);
        return -1;
    }

    close(sfd);
    return 0;
}

static int cmd_start(int argc, char *argv[]) {
    control_request_t req;
    if (argc < 5) return 1;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;

    control_response_t resp;
    if (send_control_request(&req, &resp) == 0) {
        printf("%s\n", resp.message);
        return resp.status;
    }
    return 1;
}

static int global_run_interrupted = 0;
static char global_run_id[CONTAINER_ID_LEN];

static void run_sigint_handler(int sig) {
    if (global_run_interrupted) return;
    global_run_interrupted = 1;
    control_request_t stop_req;
    memset(&stop_req, 0, sizeof(stop_req));
    stop_req.kind = CMD_STOP;
    strncpy(stop_req.container_id, global_run_id, sizeof(stop_req.container_id)-1);
    control_response_t r;
    send_control_request(&stop_req, &r);
}

static int cmd_run(int argc, char *argv[]) {
    control_request_t req;
    if (argc < 5) return 1;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;

    strncpy(global_run_id, req.container_id, CONTAINER_ID_LEN - 1);
    struct sigaction sa;
    sa.sa_handler = run_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    if (write(sfd, &req, sizeof(req)) != sizeof(req)) return 1;

    control_response_t resp;
    while (1) {
        int n = read(sfd, &resp, sizeof(resp));
        if (n > 0) {
            printf("%s\n", resp.message);
            close(sfd);
            return resp.status;
        } else if (n < 0 && errno == EINTR) {
             continue; // wait for response after stop sent
        } else {
             break;
        }
    }
    close(sfd);
    return 1;
}

static int cmd_ps(void) {
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    control_response_t resp;
    if (send_control_request(&req, &resp) == 0) {
        printf("%s", resp.message);
        return resp.status;
    }
    return 1;
}

static int cmd_logs(int argc, char *argv[]) {
    if (argc < 3) return 1;
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    control_response_t resp;
    if (send_control_request(&req, &resp) == 0) {
        printf("%s", resp.message);
        return resp.status;
    }
    return 1;
}

static int cmd_stop(int argc, char *argv[]) {
    if (argc < 3) return 1;
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    control_response_t resp;
    if (send_control_request(&req, &resp) == 0) {
        printf("%s\n", resp.message);
        return resp.status;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    if (strcmp(argv[1], "logs") == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0) return cmd_stop(argc, argv);
    usage(argv[0]);
    return 1;
}
