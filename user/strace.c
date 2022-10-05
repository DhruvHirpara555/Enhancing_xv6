#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    if (argc < 3){
        fprintf(2, "Usage: strace <mask> <command> <args...>");
        exit(1);
    }
    uint32 mask = atoi(argv[1]);
    trace(mask);
    char *cmd = argv[2];
    char *args[argc - 2];
    for (int i = 0; i < argc - 2; i++){
        args[i] = argv[i + 2];
    }
    args[argc - 2] = 0;

    exec(cmd, args);
    fprintf(2, "exec failed");
    exit(1);
}