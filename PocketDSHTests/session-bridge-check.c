// Exercises the bridge session API the way the app will: start, send commands one at a time,
// wait for each marker, read the output file between calls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "E2Bridge.h"

static char *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { *len = 0; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = calloc(1, (size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { /* short read is fine */ }
    fclose(f); *len = n; return b;
}

int main(void) {
    const char *out = "/tmp/bt.out", *mark = "/tmp/bt.mark";
    unlink(out); unlink(mark);
    if (e2_bridge_session_start("/tmp/ftest", "/tmp/bt-work", out, mark, "/tmp/ftest/bin/busybox") != 0) {
        printf("start failed\n"); return 1;
    }
    const char *cmds[] = { "cd /tmp", "pwd", "X=hello", "echo \"X=$X\"", "cd /workspace", "pwd" };
    long seen = 0;
    for (int i = 0; i < 6; i++) {
        if (e2_bridge_session_write(cmds[i]) != 0) { printf("write %d failed\n", i); return 1; }
        const int rc = e2_bridge_session_wait(i + 1, 20000, mark);
        if (rc != 0) { printf("wait %d -> %d\n", i, rc); return 1; }
        long len = 0;
        char *all = slurp(out, &len);
        if (all != NULL) {
            printf("[cmd %d] %.*s", i + 1, (int)(len - seen), all + seen);
            seen = len;
            free(all);
        }
    }
    e2_bridge_session_close();
    return 0;
}
