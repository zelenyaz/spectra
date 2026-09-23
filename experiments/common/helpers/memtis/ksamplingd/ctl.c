#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <string.h>
#include <sys/wait.h>

int syscall_htmm_start = 449;
int syscall_htmm_end = 450;

long htmm_start(pid_t pid, int node)
{
    return syscall(syscall_htmm_start, pid, node);
}

long htmm_end(pid_t pid)
{
    return syscall(syscall_htmm_end, pid);
}

int main(int argc, char** argv)
{
    long ret;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s on|off\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "on") == 0) {
        ret = htmm_start(-1, 1);
    } else if (strcmp(argv[1], "off") == 0) {
        ret = htmm_end(-1);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        fprintf(stderr, "Usage: %s on|off\n", argv[0]);
        return 1;
    }

    if (ret < 0) {
        err(1, "htmm syscall failed");
    }

    return 0;
}
