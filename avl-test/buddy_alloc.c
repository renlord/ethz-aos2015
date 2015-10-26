#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define true (1==1)
#define false (1==0)
#define bool char

#define FREE_SPACE 1UL << 30
#define START_ADDR 0
#define MIN_NODE_SIZE 1UL << 15
#define ALLOCED_BLOB(n) (n->allocated && !n->left && !n->right)

typedef unsigned long lvaddr_t;
typedef unsigned long size_t;

typedef enum {NON_LEAF, LEAF} node_type;

struct node {
    lvaddr_t addr;
    size_t max_size;
    bool allocated;
    struct node *left;
    struct node *right;
};

struct node **db;

// struct to store the paging status of a process
struct paging_state {
    struct node *root;
};


struct node *create_node() {
    struct node *n = (struct node *)malloc(sizeof(struct node));
    n->max_size = 0;
    n->addr = 0;
    n->allocated = false;
    n->left = NULL;
    n->right = NULL;
    return n;
}

void remove_node(struct node *n){
    free(n);
}

size_t get_max(struct node *x, struct node *y){
    size_t xs = ALLOCED_BLOB(x) ? 0 : x->max_size;
    size_t ys = ALLOCED_BLOB(y) ? 0 : y->max_size;
    return xs > ys ? xs : ys;
}

/**
 * Allocate memory
 */
lvaddr_t alloc_mem(struct node *cur, size_t req_size)
{   
    // We're either a leaf or have two children
    assert((!cur->left && !cur->right) || (cur->left && cur->right));
    
    // Size available in subtree is less than what's requested
    if (cur->max_size < req_size){
        printf("Cannot find memory of size %lu at node 0x%08x\n",
            req_size, (unsigned int)cur);
        printf("Maxsize for node: %lu\n", cur->max_size);
        printf("Left: 0x%08x\n", cur->left);
        printf("Right: 0x%08x\n", cur->right);
        assert(0);
    }
        
    // Case 1: Node is a leaf (i.e. blob of memory)
    if (!cur->left){
        
        assert(!cur->allocated);
        
        // Case 1a: Blob is big enough to do a split and recurse
        if ((cur->max_size >> 1) > req_size &&
            (cur->max_size >> 1) >= MIN_NODE_SIZE) {
            size_t half_size = cur->max_size >> 1;
                
            struct node *left_buddy = create_node();
            left_buddy->allocated = false;
            left_buddy->addr = cur->addr;
            left_buddy->max_size = half_size;
            
            struct node *right_buddy = create_node();
            right_buddy->allocated = false;
            right_buddy->addr = cur->addr ^ half_size;
            right_buddy->max_size = half_size;
            
            cur->left = left_buddy;
            cur->right = right_buddy;
            cur->max_size = half_size;
            cur->allocated = true;
            
            assert(left_buddy->addr < right_buddy->addr);
            
            return alloc_mem(left_buddy, req_size);
            
        // Case 1b: Blob is best fit, so mark as allocated and return address
        } else {
            cur->allocated = true;
            return cur->addr;
        }

    // Case 2: Node is intermediate, so locate the child that fits the best
    } else {
        struct node *best_fit;
        
        bool left_to_small = cur->left->max_size < req_size;
        bool right_better_fit =
            ((cur->left->max_size > cur->right->max_size) &&
            (cur->right->max_size >= req_size));
        
        if (ALLOCED_BLOB(cur->left) || left_to_small || right_better_fit){
            if(cur->right->max_size == 0){
                printf("ERROR! cur->left->max_size: %lu\n",
                    cur->left->max_size);
                printf("ALLOCED_BLOB(cur->left): %d\n",
                    ALLOCED_BLOB(cur->left));
                printf("cur->left->max_size > cur->right->max_size: %d\n",
                    cur->left->max_size > cur->right->max_size);
                assert(0);
            }
            best_fit = cur->right;
        } else {
            assert(cur->left->max_size);
            best_fit = cur->left;
        }

        lvaddr_t addr = alloc_mem(best_fit, req_size);
        cur->max_size = get_max(cur->left, cur->right);
        assert(ALLOCED_BLOB(cur->left) || cur->max_size >= cur->left->max_size);
        assert(ALLOCED_BLOB(cur->right) || cur->max_size >= cur->right->max_size);
        return addr;
    }    
}

void dealloc_mem(struct node *cur, lvaddr_t addr) 
{
    assert(cur);

    if (ALLOCED_BLOB(cur)) {
        assert(cur->addr == addr);
        cur->allocated = false;
        return;        
    } else if (cur->right && (cur->right->addr <= addr)) {
        dealloc_mem(cur->right, addr);
    } else {
        dealloc_mem(cur->left, addr);
    }
    
    assert(cur->left && cur->right);

    if (!cur->left->allocated && !cur->right->allocated){
        size_t new_size = cur->addr ^ cur->right->addr;
        
        remove_node(cur->left);
        remove_node(cur->right);
        
        cur->left = NULL;
        cur->right = NULL;
        cur->allocated = false;
        cur->max_size = new_size;
    } else {
        cur->max_size = get_max(cur->left, cur->right);
    }
}

int _print_t(struct node *tree, int is_left, int offset, int depth, char s[20][255*4])
{
    char b[20];
    int width = 24;

    if (!tree) return 0;

    sprintf(b, "%08lx (%08lx)", tree->addr, tree->max_size);

    int left  = _print_t(tree->left,  1, offset,                depth + 1, s);
    int right = _print_t(tree->right, 0, offset + left + width, depth + 1, s);


    for (int i = 0; i < width; i++)
        s[2 * depth][offset + left + i] = b[i];

    if (depth && is_left) {

        for (int i = 0; i < width + right; i++)
            s[2 * depth - 1][offset + left + width/2 + i] = '-';

        s[2 * depth - 1][offset + left + width/2] = '+';
        s[2 * depth - 1][offset + left + width + right + width/2] = '+';

    } else if (depth && !is_left) {

        for (int i = 0; i < left + width; i++)
            s[2 * depth - 1][offset - width/2 + i] = '-';

        s[2 * depth - 1][offset + left + width/2] = '+';
        s[2 * depth - 1][offset - width/2 - 1] = '+';
    }
    return left + width + right;
}

void print_t(struct node *tree)
{
    char s[40][255*4];
    for (int i = 0; i < 20; i++)
        sprintf(s[i], "%80s", " ");

    _print_t(tree, 0, 0, 0, s);

    for (int i = 0; i < 20; i++)
        printf("%s\n", s[i]);
}

void my_assertion(struct node *n) {
    if (true || !n->left || !n->right)
        return;
    
    if (n->max_size < n->left->max_size){
        printf("\nn->max_size: %lu, n->left->max_size: %lu\n",
            n->max_size, n->left->max_size);
        printf("n->addr: %lu, n->left->addr: %lu\n",
            n->addr, n->left->addr);
        assert(0);
    }

    if (n->max_size < n->right->max_size){
        printf("\nn->max_size: %lu, n->right->max_size: %lu\n",
            n->max_size, n->right->max_size);
        printf("n->addr: %lu, n->right->addr: %lu\n",
            n->addr, n->right->addr);
        assert(0);
    }

    my_assertion(n->left);
    my_assertion(n->right);
}

int main(int argc, char **argv) {
    struct paging_state *s =
        (struct paging_state *) malloc(sizeof(struct paging_state));

    // Create Pointer to Pointer to the Root Node.
    s->root = create_node();
    s->root->max_size = FREE_SPACE;
    s->root->addr = START_ADDR;
    
    db = &s->root; // TODO remove

    int verbose = atoi(argv[1]);
    int no = atoi(argv[2]);

    long addrs[no];
    long size;
    long acc = 0;
    
    for(uint32_t i = 0; i < no; i++){
        scanf("%lu", &size);
        addrs[i] = alloc_mem(s->root, size);

        printf("Allocated addr 0x%08lx for size %lu\n", addrs[i], size);
        acc += size;
        printf("acc: %lu\n", acc);
        printf("   / %lu\n", FREE_SPACE);
        printf("max: %lu\n", s->root->max_size);
        
        my_assertion(s->root);
    }
    
    if (verbose)
        printf("\nALLOCATION TRACE COMPLETE\n");

    for (uint32_t i = 0; i < no; i++) {
        dealloc_mem(s->root, addrs[i]);
        if (verbose)
            printf("Removed %lu\n", addrs[i]);
    }

    if (verbose)
        printf("Testing Complete\n");

}
