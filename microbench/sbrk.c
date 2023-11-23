// https://github.com/atecle/pa6/blob/master/mymalloc.h
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct MemEntry MemEntry;

struct MemEntry {

    struct MemEntry *prev;
    struct MemEntry *succ;
    unsigned int size;
    int isfree;
};

void *my_malloc(unsigned int size);
void my_free(void *ptr);
void remove_from_arr(MemEntry *p);

// Full-scale malloc() implementation using sbrk().
static struct MemEntry *root = 0;
static struct MemEntry *last = 0;
void *arr[5000];
static int i = 0;

void remove_from_arr(MemEntry *p) {
    int j;
    for (j = 0; j < i; j++) {
        if (p == arr[j]) {
            arr[j] = NULL;
            return;
        }
    }
}

void *my_malloc(unsigned int size) {

    struct MemEntry *p;
    struct MemEntry *succ;

    p = root;
    while (p != 0) {
        if (p->size < size) {
            p = p->succ; // too small
        } else if (!p->isfree) {
            p = p->succ; // in use
        } else if (p->size < (size + sizeof(struct MemEntry))) {
            p->isfree = 0; // too small to chop up
            remove_from_arr(p);
            return (char *)p + sizeof(struct MemEntry);
        } else {
            succ = (struct MemEntry *)((char *)p + sizeof(struct MemEntry) + size);
            succ->prev = p;
            succ->succ = p->succ;
            // p->succ->prev = succ;
            // begin add
            if (p->succ != 0)
                p->succ->prev = succ;
            // end add
            p->succ = succ;
            succ->size = p->size - sizeof(struct MemEntry) - size;
            succ->isfree = 1;
            p->size = size;
            p->isfree = 0;
            last = (p == last) ? succ : last;
            remove_from_arr(p);
            return (char *)p + sizeof(struct MemEntry);
        }
    }
    if ((p = (struct MemEntry *)sbrk(sizeof(struct MemEntry) + size)) == (void *)-1) {
        return 0;
    } else if (last == 0) // first block created
    {
        printf("BKR making first chunk size %d\n", size);
        p->prev = 0;
        p->succ = 0;
        p->size = size;
        p->isfree = 0;
        root = last = p;
        return (char *)p + sizeof(struct MemEntry);
    } else // other blocks appended
    {
        printf("BKR making another chunk size %d\n", size);
        p->prev = last;
        p->succ = last->succ;
        p->size = size;
        p->isfree = 0;
        last->succ = p;
        last = p;
        return (char *)p + sizeof(struct MemEntry);
    }
    return 0;
}

void my_free(void *p) {
    struct MemEntry *ptr;
    struct MemEntry *pred;
    struct MemEntry *succ;

    ptr = (struct MemEntry *)((char *)p - sizeof(struct MemEntry));

    struct MemEntry *temp = root;

    while (root != 0) {
        if (ptr == root) {
            break;
        }
        root = root->succ;
        if (root == 0) {
            printf("<ERROR>: Has not been malloc or freed already\n");
            root = temp;
            return;
        }
    }

    int j;
    for (j = 0; j < i; j++) {
        if (ptr == arr[j]) {
            printf("<ERROR>: Has already been freed\n");
            return;
        }
    }

    root = temp;

    if ((pred = ptr->prev) != 0 && pred->isfree) {
        pred->size += sizeof(struct MemEntry) + ptr->size; // merge with predecessor

        pred->succ = ptr->succ;
        // begin added
        ptr->isfree = 1;
        pred->succ = ptr->succ;
        if (ptr->succ != 0)
            ptr->succ->prev = pred;
        // end added
        printf("BKR freeing block %#p merging with predecessor new size is %d.\n", p, pred->size);
    } else {
        printf("BKR freeing block %#p.\n", p);
        arr[i++] = ptr;
        ptr->isfree = 1;
        pred = ptr;
    }
    if ((succ = ptr->succ) != 0 && succ->isfree) {
        pred->size += sizeof(struct MemEntry) + succ->size; // merge with successor
        pred->succ = succ->succ;
        // begin added
        pred->isfree = 1;

        if (succ->succ != 0)
            succ->succ->prev = pred;
        // end added
        arr[i++] = ptr;
        printf("BKR freeing block %#p merging with successor new size is %d.\n", p, pred->size);
    }
}

int main(int argc, const char *const *argv) {

    size_t mbcount = 100;

    printf("allocating %ld MB\n", mbcount);
    uint8_t *p;
    p = (uint8_t *)my_malloc(mbcount * 1024ULL * 1024ULL);

    if (p == NULL) {
        fprintf(stderr, "sbrk() failed: %s", strerror(errno));
        return -1;
    }

    printf("filling\n");
    for (size_t i = 0; i < mbcount * 1024ULL * 1024ULL; i++) {
        p[i] = 'w';
    }
    printf("freeing\n");
    my_free(p);

    return 0;
}