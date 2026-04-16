#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
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
#include <unistd.h>

#include "monitor_ioctl.h"

#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define MAX_CONTAINERS 100
#define CONTAINER_ID_LEN 32
#define CHILD_CMD_LEN 256

typedef enum {
    CMD_START = 1,
    CMD_RUN,
    CMD_PS,
    CMD_STOP
} command_kind_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_CMD_LEN];
} control_request_t;

typedef struct {
    int status;
    char message[512];
} control_response_t;

typedef struct {
    char id[64];
    pid_t pid;
    char status[20];
} container_t;

container_t containers[MAX_CONTAINERS];
int container_count = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- LOGGING ---------------- */

typedef struct {
    int fd;
    char container_id[CONTAINER_ID_LEN];
} log_args_t;

void *log_reader(void *arg)
{
    log_args_t *a = (log_args_t *)arg;
    char buf[4096];

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, a->container_id);

    int out = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (out < 0) return NULL;

    while (1) {
        ssize_t n = read(a->fd, buf, sizeof(buf));
        if (n <= 0) break;
        write(out, buf, n);
    }

    close(out);
    close(a->fd);
    free(a);
    return NULL;
}

/* ---------------- SUPERVISOR LOG ---------------- */

void sup_log(const char *msg)
{
    printf("[SUPERVISOR] %s\n", msg);
    fflush(stdout);
}

/* ---------------- SIGCHLD ---------------- */

void handle_sigchld(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&lock);
        for (int i = 0; i < container_count; i++) {
            if (containers[i].pid == pid) {
                strcpy(containers[i].status, "STOPPED");
                break;
            }
        }
        pthread_mutex_unlock(&lock);
    }
}

/* ---------------- SUPERVISOR ---------------- */

int run_supervisor()
{
    int server_fd, client_fd;
    struct sockaddr_un addr;

    mkdir(LOG_DIR, 0755);
    signal(SIGCHLD, handle_sigchld);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(CONTROL_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 10);

    sup_log("Supervisor running...");

    while (1)
    {
        control_request_t req;
        control_response_t resp;

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        read(client_fd, &req, sizeof(req));

        memset(&resp, 0, sizeof(resp));
        resp.status = 0;

        /* ---------------- START ---------------- */
        if (req.kind == CMD_START || req.kind == CMD_RUN)
        {
            int pipefd[2];
            pipe(pipefd);

            pid_t pid = fork();

            if (pid == 0)
            {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);

                if (chroot(req.rootfs) != 0) {
                    perror("chroot");
                    exit(1);
                }

                chdir("/");

                mkdir("/proc", 0555);
                mount("proc", "/proc", "proc", 0, NULL);

                // SAFE EXEC PATH
                char exec_path[PATH_MAX];

                if (req.command[0] == '/') {
                    strncpy(exec_path, req.command, sizeof(exec_path) - 1);
                    exec_path[sizeof(exec_path) - 1] = '\0';
                } else {
                    if (snprintf(exec_path, sizeof(exec_path), "/%s", req.command) >= sizeof(exec_path)) {
                        fprintf(stderr, "Command path too long\n");
                        exit(1);
                    }
                }

                execl(exec_path, exec_path, NULL);

                perror("exec failed");
                exit(1);
            }

            close(pipefd[1]);

            log_args_t *args = malloc(sizeof(log_args_t));
            args->fd = pipefd[0];
            strncpy(args->container_id, req.container_id, CONTAINER_ID_LEN);

            pthread_t tid;
            pthread_create(&tid, NULL, log_reader, args);
            pthread_detach(tid);

            pthread_mutex_lock(&lock);
            strcpy(containers[container_count].id, req.container_id);
            containers[container_count].pid = pid;
            strcpy(containers[container_count].status, "RUNNING");
            container_count++;
            pthread_mutex_unlock(&lock);

            char msg[256];
            snprintf(msg, sizeof(msg),
                     "started %s pid=%d",
                     req.container_id, pid);
            sup_log(msg);

            int fd = open("/dev/container_monitor", O_RDWR);

            if (fd >= 0) {
                struct monitor_request mreq;

                mreq.pid = pid;
                strcpy(mreq.container_id, req.container_id);

                mreq.soft_limit_bytes = 5 * 1024 * 1024;
                mreq.hard_limit_bytes = 10 * 1024 * 1024;

                ioctl(fd, MONITOR_REGISTER, &mreq);
                close(fd);

                sup_log("registered with kernel monitor");
            }
        }

        /* ---------------- PS ---------------- */
        else if (req.kind == CMD_PS)
        {
            char out[1024];
            out[0] = 0;

            strcat(out, "ID\tPID\tSTATUS\n");

            pthread_mutex_lock(&lock);
            for (int i = 0; i < container_count; i++)
            {
                char line[128];
                snprintf(line, sizeof(line),
                         "%s\t%d\t%s\n",
                         containers[i].id,
                         containers[i].pid,
                         containers[i].status);
                strcat(out, line);
            }
            pthread_mutex_unlock(&lock);

            strcpy(resp.message, out);
            sup_log(out);
        }

        /* ---------------- STOP ---------------- */
        else if (req.kind == CMD_STOP)
        {
            pthread_mutex_lock(&lock);

            for (int i = 0; i < container_count; i++)
            {
                if (strcmp(containers[i].id, req.container_id) == 0)
                {
                    kill(containers[i].pid, SIGKILL);
                    strcpy(containers[i].status, "STOPPED");
                }
            }

            pthread_mutex_unlock(&lock);
        }

        write(client_fd, &resp, sizeof(resp));
        close(client_fd);
    }
}

/* ---------------- CLIENT ---------------- */

int send_request(control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(fd, (struct sockaddr *)&addr, sizeof(addr));

    write(fd, req, sizeof(*req));

    control_response_t resp;
    read(fd, &resp, sizeof(resp));

    printf("%s\n", resp.message);

    close(fd);
    return 0;
}

/* ---------------- MAIN ---------------- */

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("usage: supervisor | start | ps | stop\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    control_request_t req;
    memset(&req, 0, sizeof(req));

    if (strcmp(argv[1], "start") == 0)
    {
        req.kind = CMD_START;
        strcpy(req.container_id, argv[2]);
        strcpy(req.rootfs, argv[3]);
        strcpy(req.command, argv[4]);
    }
    else if (strcmp(argv[1], "ps") == 0)
    {
        req.kind = CMD_PS;
    }
    else if (strcmp(argv[1], "stop") == 0)
    {
        req.kind = CMD_STOP;
        strcpy(req.container_id, argv[2]);
    }

    return send_request(&req);
}