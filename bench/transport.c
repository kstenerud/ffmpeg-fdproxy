// Measures the cost of the transport between a parent serving reads and FFmpeg's side, apart from
// any disk: a file in the page cache is read in blocks by a "server" (the node) and consumed by a
// "client" (FFmpeg's protocol) that copies it out 32 KiB at a time, as FFmpeg's I/O layer asks.
//   direct  - the client reads the file itself (today), no server
//   pipe    - the server preads a block into its buffer and writes a header and the block to a pipe
//   splice  - the server writes the header, then splices the block from the file into the pipe
//   shm     - the server preads the block into shared memory (a memfd) and writes a 16-byte header;
//             the client copies out of the shared memory
// Usage: transport <file> <mode> <block bytes> <pipe bytes> ; prints MB/s and CPU-seconds per GB.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONSUME 32768
struct req { int64_t offset, length; };
struct ans { int64_t offset, length; };

static void full_write(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) { ssize_t w = write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; perror("write"); exit(1); } p += w; n -= w; }
}
static void full_read(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) { ssize_t r = read(fd, p, n); if (r < 0) { if (errno == EINTR) continue; perror("read"); exit(1); } if (r == 0) { fprintf(stderr, "eof\n"); exit(1); } p += r; n -= r; }
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static volatile uint64_t sink;
static void consume(const char *data, int64_t n, char *avio) {
    for (int64_t at = 0; at < n; at += CONSUME) {
        int64_t k = n - at < CONSUME ? n - at : CONSUME;
        memcpy(avio, data + at, k);
        sink += (unsigned char)avio[k - 1];
    }
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s file mode block pipebytes\n", argv[0]); return 2; }
    const char *path = argv[1], *mode = argv[2];
    int64_t block = atoll(argv[3]); int pipebytes = atoi(argv[4]);
    int file = open(path, O_RDONLY); struct stat st; fstat(file, &st); int64_t size = st.st_size;
    char *avio = malloc(CONSUME), *buf = malloc(block);
    double start = now();
    if (!strcmp(mode, "direct")) {
        for (int64_t at = 0; at < size;) { ssize_t r = read(file, avio, CONSUME); if (r <= 0) break; at += r; sink += (unsigned char)avio[0]; }
    } else {
        int toServer[2], toClient[2]; pipe(toServer); pipe(toClient);
        if (pipebytes > 0) { fcntl(toClient[1], F_SETPIPE_SZ, pipebytes); fcntl(toServer[1], F_SETPIPE_SZ, pipebytes); }
        int shm = -1; char *shared = NULL;
        if (!strcmp(mode, "shm")) { shm = memfd_create("blocks", 0); ftruncate(shm, block); shared = mmap(NULL, block, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0); }
        pid_t client = fork();
        if (client == 0) {  // FFmpeg's side
            close(toServer[0]); close(toClient[1]);
            for (int64_t at = 0; at < size; at += block) {
                struct req q = { at, size - at < block ? size - at : block };
                full_write(toServer[1], &q, sizeof q);
                struct ans a; full_read(toClient[0], &a, sizeof a);
                if (shared) consume(shared, a.length, avio);
                else { full_read(toClient[0], buf, a.length); consume(buf, a.length, avio); }
            }
            _exit(0);
        }
        close(toServer[1]); close(toClient[0]);  // the node's side
        for (int64_t served = 0; served < size;) {
            struct req q; full_read(toServer[0], &q, sizeof q);
            struct ans a = { q.offset, q.length };
            if (!strcmp(mode, "pipe")) {
                if (pread(file, buf, q.length, q.offset) != q.length) { perror("pread"); exit(1); }
                full_write(toClient[1], &a, sizeof a); full_write(toClient[1], buf, q.length);
            } else if (!strcmp(mode, "splice")) {
                full_write(toClient[1], &a, sizeof a);
                loff_t off = q.offset; int64_t left = q.length;
                while (left > 0) { ssize_t s = splice(file, &off, toClient[1], NULL, left, SPLICE_F_MOVE); if (s <= 0) { perror("splice"); exit(1); } left -= s; }
            } else {
                if (pread(file, shared, q.length, q.offset) != q.length) { perror("pread"); exit(1); }
                full_write(toClient[1], &a, sizeof a);
            }
            served += q.length;
        }
        int status; waitpid(client, &status, 0);
    }
    double took = now() - start;
    struct rusage self, kids; getrusage(RUSAGE_SELF, &self); getrusage(RUSAGE_CHILDREN, &kids);
    double cpu = self.ru_utime.tv_sec + self.ru_utime.tv_usec / 1e6 + self.ru_stime.tv_sec + self.ru_stime.tv_usec / 1e6
               + kids.ru_utime.tv_sec + kids.ru_utime.tv_usec / 1e6 + kids.ru_stime.tv_sec + kids.ru_stime.tv_usec / 1e6;
    double gb = size / 1e9;
    printf("%-7s block %5lld KiB pipe %5d KiB: %6.0f MB/s, %.2f CPU-s per GB\n", mode, (long long)(block >> 10), pipebytes >> 10, size / 1e6 / took, cpu / gb);
    return 0;
}
