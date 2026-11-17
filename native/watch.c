#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#ifdef __ANDROID__
#include <android/log.h>
#define SHELL "/system/bin/sh"
#else
#define SHELL "/bin/sh"
#endif

#define TABLE 52
#define PROTO 242
#define DRAIN_LIMIT 64
#define WORK_TIMEOUT_MS 20000

static volatile sig_atomic_t quitting;
static void on_signal(int sig) { (void)sig; quitting = 1; }

static void report(const char *s) {
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "TS-LAN-Route", s);
#else
    fprintf(stderr, "%s\n", s);
#endif
}

static bool path_join(char *out, const char *dir, const char *name) {
    int n = snprintf(out, PATH_MAX, "%s/%s", dir, name);
    return n >= 0 && n < PATH_MAX;
}

static bool has_file(const char *dir, const char *name) {
    char path[PATH_MAX];
    return path_join(path, dir, name) && access(path, F_OK) == 0;
}

static bool private_dst(uint32_t addr) {
    uint32_t a = ntohl(addr);
    return (a >> 24) == 10 || (a >> 20) == 0xac1 ||
        (a >> 16) == 0xc0a8 || (a >> 16) == 0xa9fe;
}

static bool excluded_interface(unsigned int index) {
    static const char *const prefixes[] = {
        "lo", "dummy", "tailscale", "tun", "tap", "wg", "rmnet",
        "ccmni", "pdp", "wwan", "v4-", "clat", "ipsec", "xfrm",
        "ip6tnl", "sit", "gre", "gretap", "erspan", "veth", "docker", "podman"
    };
    char name[IF_NAMESIZE];
    if (!index || !if_indextoname(index, name)) return false;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) return true;
    return false;
}

static bool relevant_route(const struct nlmsghdr *h) {
    if (h->nlmsg_type == NLMSG_OVERRUN) return true;
    if (h->nlmsg_type != RTM_NEWROUTE && h->nlmsg_type != RTM_DELROUTE) return false;
    if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtmsg))) return false;
    const struct rtmsg *r = NLMSG_DATA(h);
    if (r->rtm_family != AF_INET) return false;
    uint32_t table = r->rtm_table, dst = 0, oif = 0;
    int len = RTM_PAYLOAD(h);
    for (const struct rtattr *a = RTM_RTA(r); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
        if (RTA_PAYLOAD(a) < sizeof(uint32_t)) continue;
        if (a->rta_type == RTA_TABLE) memcpy(&table, RTA_DATA(a), sizeof(table));
        if (a->rta_type == RTA_DST) memcpy(&dst, RTA_DATA(a), sizeof(dst));
        if (a->rta_type == RTA_OIF) memcpy(&oif, RTA_DATA(a), sizeof(oif));
    }
    if (table == TABLE) {
        if (r->rtm_protocol == PROTO && h->nlmsg_type == RTM_NEWROUTE) return false;
        return r->rtm_dst_len == 0 || private_dst(dst);
    }
    return table != RT_TABLE_LOCAL && r->rtm_scope == RT_SCOPE_LINK &&
        r->rtm_type == RTN_UNICAST && private_dst(dst) && !excluded_interface(oif);
}

static int open_netlink(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) return -1;
    int buffer = 256 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_IPV4_ROUTE };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
    close(fd);
    return -1;
}

static int drain_routes(int fd) {
    bool relevant = false;
    union {
        struct nlmsghdr align;
        char bytes[32768];
    } buf;
    for (int i = 0; i < DRAIN_LIMIT; ++i) {
        struct sockaddr_nl from = {0};
        socklen_t size = sizeof(from);
        ssize_t n = recvfrom(fd, buf.bytes, sizeof(buf.bytes), MSG_TRUNC,
            (struct sockaddr *)&from, &size);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) { relevant = true; continue; }
            return -1;
        }
        if (from.nl_pid != 0) continue;
        if ((size_t)n > sizeof(buf.bytes)) { relevant = true; continue; }
        unsigned int len = (unsigned int)n;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf.bytes;
            NLMSG_OK(h, len); h = NLMSG_NEXT(h, len)) {
            if (relevant_route(h)) relevant = true;
        }
    }
    return relevant;
}

struct watches {
    int parent, ts, tmp, module, state;
    const char *parent_dir, *ts_dir, *tmp_dir, *mod_dir, *state_dir;
};

static int watch_dir(int fd, const char *path) {
    return inotify_add_watch(fd, path, IN_CREATE | IN_DELETE | IN_MOVED_FROM |
        IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
}

static void rearm(int fd, struct watches *w) {
    w->parent = watch_dir(fd, w->parent_dir);
    w->ts = watch_dir(fd, w->ts_dir);
    w->tmp = watch_dir(fd, w->tmp_dir);
    w->module = watch_dir(fd, w->mod_dir);
    w->state = watch_dir(fd, w->state_dir);
}

static bool ts_name(const char *name) {
    return strcmp(name, "tailscaled.state") == 0 ||
        strcmp(name, "tailscaled.sock") == 0 || strcmp(name, "tmp") == 0;
}

static int drain_files(int fd, struct watches *w) {
    union {
        struct inotify_event align;
        char bytes[8192];
    } buf;
    bool relevant = false, refresh = false;
    for (int i = 0; i < DRAIN_LIMIT; ++i) {
        ssize_t n = read(fd, buf.bytes, sizeof(buf.bytes));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return -1;
        }
        for (size_t off = 0; off < (size_t)n;) {
            const struct inotify_event *e = (const void *)(buf.bytes + off);
            off += sizeof(*e) + e->len;
            if (e->mask & (IN_Q_OVERFLOW | IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                relevant = refresh = true;
                continue;
            }
            if (!e->len) continue;
            const char *name = e->name;
            if ((e->wd == w->parent && strcmp(name, "tailscale") == 0) ||
                ((e->wd == w->ts || e->wd == w->tmp) && ts_name(name))) {
                relevant = refresh = true;
            } else if ((e->wd == w->module &&
                (strcmp(name, "disable") == 0 || strcmp(name, "remove") == 0)) ||
                (e->wd == w->state && strcmp(name, "watch.stop") == 0)) {
                relevant = true;
            }
        }
    }
    if (refresh) rearm(fd, w);
    return relevant;
}

static int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int run_apply(const char *script) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return -1;
    pid_t child = fork();
    if (child == 0) {
        (void)setpgid(0, 0);
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0 || dup2(p[1], STDERR_FILENO) < 0) _exit(126);
        close(p[1]);
        execl(SHELL, SHELL, script, "apply", (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (child < 0) { close(p[0]); return -1; }
    (void)setpgid(child, child);
    int64_t deadline = now_ms() + WORK_TIMEOUT_MS;
    char line[1024], buf[2048];
    size_t used = 0;
    bool timed_out = false;
    for (;;) {
        int64_t remaining = deadline - now_ms();
        if (quitting || remaining <= 0) { timed_out = true; break; }
        struct pollfd fd = { .fd = p[0], .events = POLLIN };
        int result = poll(&fd, 1, (int)remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) { timed_out = true; break; }
        ssize_t n = read(p[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                line[used] = '\0';
                if (!strncmp(line, "[ADD]", 5) || !strncmp(line, "[REMOVE]", 8) ||
                    !strncmp(line, "[ERROR]", 7)) report(line);
                used = 0;
            } else if (used < sizeof(line) - 1) {
                line[used++] = buf[i];
            }
        }
    }
    close(p[0]);
    if (timed_out) {
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
        report("Automatic apply interrupted or timed out");
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return !timed_out && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static bool stopped(const char *mod, const char *state) {
    return quitting || has_file(state, "watch.stop") || has_file(mod, "disable") ||
        has_file(mod, "remove") || access(mod, F_OK) != 0;
}

static int lock_file(const char *dir) {
    char path[PATH_MAX];
    if (!path_join(path, dir, "watch.lock")) return -1;
    return open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
}

static int stop_watch(const char *dir) {
    char path[PATH_MAX];
    if (!path_join(path, dir, "watch.stop")) return 1;
    int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return 1;
    close(fd);
    fd = lock_file(dir);
    if (fd < 0) return 1;
    int result;
    do { result = flock(fd, LOCK_EX); } while (result < 0 && errno == EINTR);
    close(fd);
    return result == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "watch") && strcmp(argv[1], "stop"))) return 2;
    const char *mod = argv[2], *state = argv[3];
    umask(077);
    if (mkdir(state, 0700) != 0 && errno != EEXIST) return 1;
    if (!strcmp(argv[1], "stop")) return stop_watch(state);
    int lock = lock_file(state);
    if (lock < 0) return 1;
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        int error = errno;
        close(lock);
        return error == EWOULDBLOCK ? 0 : 1;
    }
    if (stopped(mod, state)) { close(lock); return 0; }
    char script[PATH_MAX];
    if (!path_join(script, mod, "system/bin/ts-lan-route")) { close(lock); return 1; }
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    int net = open_netlink(), ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (net < 0 || ino < 0) {
        report("Unable to subscribe to network/state notifications");
        if (net >= 0) close(net);
        if (ino >= 0) close(ino);
        close(lock);
        return 1;
    }
    struct watches w = {
        .parent_dir = "/data/adb", .ts_dir = "/data/adb/tailscale",
        .tmp_dir = "/data/adb/tailscale/tmp", .mod_dir = mod, .state_dir = state
    };
    rearm(ino, &w);
    if (w.module < 0 || w.state < 0) {
        report("Unable to watch module/state directory");
        close(ino); close(net); close(lock);
        return 1;
    }
    report("Automatic route recovery started (event-driven)");
    bool pending = true;
    const int retry_delays[] = { 200, 1000, 3000 };
    unsigned int retries = 0;
    int64_t retry_at = -1;
    int exit_code = 0;
    while (!stopped(mod, state)) {
        if (pending) {
            pending = false;
            if (run_apply(script) == 0) {
                retries = 0;
                retry_at = -1;
            } else if (retries < sizeof(retry_delays) / sizeof(retry_delays[0])) {
                retry_at = now_ms() + retry_delays[retries++];
            } else {
                retry_at = -1;
                report("Automatic apply failed; waiting for the next network/state event");
            }
        }
        if (stopped(mod, state)) break;
        int timeout = -1;
        if (retry_at >= 0) {
            int64_t remaining = retry_at - now_ms();
            timeout = remaining > 0 ? (int)remaining : 0;
        }
        struct pollfd fds[] = {
            { .fd = net, .events = POLLIN },
            { .fd = ino, .events = POLLIN }
        };
        int n = poll(fds, 2, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            exit_code = 1; break;
        }
        if (n == 0) { retry_at = -1; pending = true; continue; }
        if ((fds[0].revents & (POLLHUP | POLLNVAL)) ||
            (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            exit_code = 1; break;
        }
        int route = fds[0].revents & (POLLIN | POLLERR) ? drain_routes(net) : 0;
        int file = fds[1].revents & POLLIN ? drain_files(ino, &w) : 0;
        if (route < 0 || file < 0) { exit_code = 1; break; }
        if (route || file) {
            pending = true;
            if (retry_at < 0) retries = 0;
            retry_at = -1;
        }
    }
    if (exit_code) report("Notification listener failed");
    report("Automatic route recovery stopped");
    close(ino); close(net); close(lock);
    return exit_code;
}
