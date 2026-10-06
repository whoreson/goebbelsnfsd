/*
 * test_mknod.c - Test FIFO and UNIX socket creation over NFS
 *
 * Usage (server side, on NFS export):
 *   ./test_mknod fifo /export/testfifo
 *   ./test_mknod socket /export/testsock
 *
 * Or over NFS mount:
 *   ./test_mknod fifo /mnt/nfs/testfifo
 *   ./test_mknod socket /mnt/nfs/testsock
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

int test_fifo(const char *path)
{
    printf("Creating FIFO: %s\n", path);
    if (mkfifo(path, 0666) < 0) {
        perror("mkfifo");
        return 1;
    }
    {
        struct stat st;
        if (stat(path, &st) == 0)
            printf("OK: mode=%o type=%s\n",
                st.st_mode & 07777,
                S_ISFIFO(st.st_mode) ? "FIFO" : "???");
    }
    unlink(path);
    return 0;
}

int test_socket(const char *path)
{
    int s;
    struct sockaddr_un addr;
    struct stat st;

    printf("Creating UNIX socket: %s\n", path);
    s = socket(PF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, path, sizeof(addr.sun_path));

    if (bind(s, (struct sockaddr *)&addr,
        sizeof(addr.sun_family) + strlen(addr.sun_path)) < 0) {
        perror("bind");
        close(s);
        return 1;
    }

    if (fstat(s, &st) == 0)
        printf("OK: mode=%o type=%s\n",
            st.st_mode & 07777,
            S_ISSOCK(st.st_mode) ? "SOCK" : "???");

    close(s);
    unlink(path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s {fifo|socket} <path>\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "fifo") == 0)
        return test_fifo(argv[2]);
    if (strcmp(argv[1], "socket") == 0)
        return test_socket(argv[2]);
    fprintf(stderr, "unknown type: %s\n", argv[1]);
    return 1;
}
