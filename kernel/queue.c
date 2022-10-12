#include "proc.h"

struct que
*que_init(void){
    struct que *que = (struct que *)kalloc();
    que->head = 0;
    que->tail = 0;
    que->size = 0;
    return que;
}

void
que_push(struct que *que, struct proc *proc){
    struct que_node *node = (struct que_node *)kalloc();
    node->proc = proc;
    node->next = 0;
    if(que->head == 0){
        que->head = node;
        que->tail = node;
    }else{
        que->tail->next = node;
        que->tail = node;
    }
    que->size++;
}

struct proc
*que_pop(struct que *que){
    if(que->head == 0){
        return 0;
    }
    struct que_node *node = que->head;
    struct proc *proc = node->proc;
    que->head = node->next;
    if(que->head == 0){
        que->tail = 0;
    }
    kfree((void *)node);
    que->size--;
    return proc;
}

struct proc
*que_front(struct que *que){
    if(que->head == 0){
        return 0;
    }
    return que->head->proc;
}

int
que_empty(struct que *que){
    return que->head == 0;
}

void
que_remove(struct que *que, struct proc *proc){
    struct que_node *node = que->head;
    struct que_node *prev = 0;
    while(node != 0){
        if(node->proc == proc){
            if(prev == 0){
                que->head = node->next;
            }else{
                prev->next = node->next;
            }
            if(node->next == 0){
                que->tail = prev;
            }
            kfree((void *)node);
            que->size--;
            return;
        }
        prev = node;
        node = node->next;
    }
}