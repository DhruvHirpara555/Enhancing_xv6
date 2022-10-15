#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    if(argc != 3){
        fprintf(2,"Usage: setpriority <priority> <pid>");
    }
    int priority = atoi(argv[1]);
    int pid = atoi(argv[2]);
    int p = setpriority(priority,pid);
    printf("setpriority returned %d",p);

    exit(0);

}