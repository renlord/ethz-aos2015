#include <stdio.h>
#include <stdlib.h>
#include <assert.h>


#define MAX(a,b) \
    ({ __typeof__ (a) _a = (a); \
     __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define MIN(a,b) \
    ({ __typeof__ (a) _a = (a); \
     __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

#define RIGHT_HEAVY(node) (node->right ? node->right->height : 0) \
    > (node->left ? node->left->height : 0)
#define LEFT_HEAVY(node) (node->left ? node->left->height : 0) \
    > (node->right ? node->right->height : 0)

#define true (1==1)
#define false (1==0)
#define bool char

typedef unsigned long lpaddr_t;
typedef unsigned long lvaddr_t;
typedef unsigned long addr_t;

typedef enum {
    V_TO_P, P_TO_V
} avl_type;

struct addr_mapping {
    lvaddr_t vaddr;
    lpaddr_t paddr;
};

struct avl_node {
    struct addr_mapping *mapping;
    struct avl_node *left;
    struct avl_node *right;
    unsigned short height; 
};   

// struct to store the paging status of a process
struct paging_state {
    struct avl_node **virt_to_phys;
    struct avl_node **phys_to_virt;
};

static void avl_update_height(struct avl_node *node) {
    assert(node != NULL);

    short lh = node->left ? node->left->height : 0;
    short rh = node->right ? node->right->height : 0;

    node->height = MAX(lh, rh) + 1;
}

static void avl_rotate_left(struct avl_node **node) 
{
    // Pre-Conditions
    assert(node != NULL && *node != NULL);
    assert((*node)->right != NULL);

    struct avl_node *temp = (*node)->right;

    (*node)->right = temp->left;
    temp->left = *node;

    // _clean_node_child_ref(node);
    avl_update_height(*node);
    avl_update_height(temp);

    *node = temp;
}

/**
 * returns the new child for the parent
 */
static void avl_rotate_right(struct avl_node **node) 
{
    // Pre-Conditions
    assert(node != NULL && *node != NULL);
    assert((*node)->left != NULL);

    struct avl_node *new_root = (*node)->left;

    (*node)->left = new_root->right;
    new_root->right = *node;

    avl_update_height(*node);
    avl_update_height(new_root);

    *node = new_root;
}

/**
 * // node, current node to balance
 * // root, the root node of the tree
 * 
 * RETURNS the new child for the parent
 */
static void avl_balance_node(struct avl_node **parent) 
{
    assert(parent != NULL);
    struct avl_node *node = *parent;

    // get balance for node
    short lh = (node->left != NULL) ? node->left->height : 0;
    short rh = (node->right != NULL) ? node->right->height : 0;
    short balance = lh - rh;

    //printf("balance %d\n", balance);
    if (balance == -1) 
    {
        // handle right subtree too high
        assert(node->right != NULL);
        if((node->right->right != NULL) && 
                (node->right->left != NULL) && 
                LEFT_HEAVY(node->right)) 
        {
            avl_rotate_right(&(node->right));
        }
        avl_rotate_left(parent);
        //printf("rotation done\n");
    } 
    else if (balance == 1) 
    {
        // handle left subtree too high
        // check for left right case
        assert(node->left != NULL);
        if((node->left->left != NULL) && 
                (node->left->right != NULL) && 
                RIGHT_HEAVY(node->left)) 
        {
            avl_rotate_left(&(node->left));
        }
        avl_rotate_right(parent);
        //printf("rotation done\n");
    } 
    else {
        ;
    }
    assert(parent != NULL);
    assert(*parent != NULL);
}

static struct avl_node* avl_lookup_aux(struct avl_node *node, avl_type type, addr_t addr){
    addr_t key = type == V_TO_P ? node->mapping->vaddr : node->mapping->paddr;
    if (!node)
        return NULL;
    else if(addr > key) {
        //printf("node value: %lu\n", node->mapping->paddr);
        return avl_lookup_aux(node->right, type, addr);
    } else if(addr < key) {
        //printf("node value: %lu\n", node->mapping->paddr);
        return avl_lookup_aux(node->left, type, addr);
    } else {
        // addr == node->key
        return node;
    }
}

addr_t avl_lookup(struct paging_state *s, addr_t addr) {
    //printf("searching %lu\n", addr);
    struct avl_node *res = avl_lookup_aux(*(s->phys_to_virt), P_TO_V, addr);
    return res->mapping->vaddr;
}

/**
 * insert node
 *
 * inserts generic node to generic tree
 * 
 * YOUR RESPONSIBILITY to provide correct tree and correct `key_type` or everything will
 * FUCK UP.
 * 
 * // cur, pointer of the current node.
 * // key_type, the key type to use, either P_TO_V or V_TO_P
 * // child, pointer to the child node.
 * // root, pointer to the root node.
 *
 * CAVEATS:
 * Assumes that lookup has been performed and confirm that the address has not been mapped.
 */
static void insert_node(struct avl_node **parent, 
        avl_type key_type, 
        struct avl_node *child)
{
    addr_t child_key, intree_key;

    if (*parent == NULL) 
    {   
        // Changes the Pointer to the Pointer of the Root Node.
        *parent = child;
    }
    else 
    {
        struct avl_node *cur = *parent;
        //printf("Traversing Node Pointer: %p | %p\n", cur, *parent);
        if (key_type == V_TO_P) {
            child_key = (addr_t) child->mapping->vaddr;
            intree_key = (addr_t) cur->mapping->vaddr;
        } else {
            child_key = (addr_t) child->mapping->paddr;
            intree_key = (addr_t) cur->mapping->paddr;
        }
        //assumes insertion successful
        if(child_key < intree_key) {
            // passes in the pointer to the pointer of the subsequent child node.
            insert_node(&(cur->left), key_type, child);
        } else {
            insert_node(&(cur->right), key_type, child);
        }
        avl_update_height(cur);
        avl_balance_node(parent);
    }
}

// There is the edge case where removing a mapping will result in another
// node being the root node instead. 
// So the function should return an avl_node pointer to inform of the newest root node.
static struct avl_node *remove_mapping(struct paging_state *s, lpaddr_t addr){
    //TODO
    // Pre-Conditions 
    assert(s != NULL && s->virt_to_phys != NULL && s->phys_to_virt != NULL);

    lvaddr_t virtualKey = avl_lookup(s, (addr_t) addr); 

    // Remove from P_TO_V


    // Remove from V_TO_P  

    // Post-Conditions

}

int _print_t(struct avl_node *tree, avl_type tree_type, int is_left, int offset, int depth, char s[20][255*4])
{
    char b[32];
    int width = 24;

    if (!tree) return 0;
    /* DEPRECATED
     * Left for reference purposes.
     struct addr_mapping *m = tree->mapping;
     addr_t key = tree->type == P_TO_V ? m->paddr : m->vaddr;
     addr_t val = tree->type == P_TO_V ? m->vaddr : m->paddr;
     sprintf(b, "%08lx -> %08lx (%i)", key, val, tree->height);
     */
    struct addr_mapping *m = tree->mapping;
    addr_t key = tree_type == P_TO_V ? m->paddr : m->vaddr;
    addr_t val = tree_type == P_TO_V ? m->vaddr : m->paddr;
    sprintf(b, "%08lx -> %08lx (%i)", key, val, tree->height);

    int left  = _print_t(tree->left,  tree_type, 1, offset,                depth + 1, s);
    int right = _print_t(tree->right, tree_type, 0, offset + left + width, depth + 1, s);


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

void print_t(struct avl_node *tree, avl_type tree_type)
{
    char s[20][255*4];
    for (int i = 0; i < 20; i++)
        sprintf(s[i], "%80s", " ");

    _print_t(tree, tree_type, 0, 0, 0, s);

    for (int i = 0; i < 20; i++)
        printf("%s\n", s[i]);
}

struct addr_mapping *create_mapping(lpaddr_t paddr, lvaddr_t vaddr){
    struct addr_mapping* m = (struct addr_mapping *) malloc(sizeof(struct addr_mapping));
    m->paddr = paddr;
    m->vaddr = vaddr;
    return m;
}

struct avl_node *create_node(struct addr_mapping *m){
    struct avl_node* n = (struct avl_node *) malloc(sizeof(struct avl_node));
    n->mapping = m;
    n->left = NULL;
    n->right = NULL;
    n->height = 0;
    return n;
}

// TODO: Revise to support Start & End addresses.
void insert_mapping(struct paging_state *s, lvaddr_t vaddr, lpaddr_t paddr){

    struct addr_mapping *m = create_mapping(vaddr, paddr);
    struct avl_node *n_p2v = create_node(m);
    struct avl_node *n_v2p = create_node(m);

    insert_node(s->phys_to_virt, P_TO_V, n_p2v);
    insert_node(s->virt_to_phys, V_TO_P, n_v2p);
}

void clean_tree(struct avl_node *node, bool free_mapping) {
    struct avl_node *left = node->left;
    struct avl_node *right = node->right;

    // only free the mappings if told to to avoid redundant freeing
    if(free_mapping)
        free(node->mapping);

    free(node);

    // recurse on each subtrees if existent
    if(left != NULL)
        clean_tree(left, free_mapping);
    if(right != NULL)
        clean_tree(right, free_mapping);
}

int main(int argc, char **argv) {
    struct paging_state *s = (struct paging_state *) malloc(sizeof(struct paging_state));
    // Create Pointer to Pointer to the Root Node.
    s->phys_to_virt = (struct avl_node **) malloc(sizeof(struct avl_node *));
    s->virt_to_phys = (struct avl_node **) malloc(sizeof(struct avl_node *));

    int verbose = atoi(argv[1]);
    int no = atoi(argv[2]);
    long cop[no];
    long x;

    for(int i = 0; i < no; i++){
        scanf("%lu", &x);
        cop[i] = x;
        insert_mapping(s, x, x);
        if(verbose){
            printf("inserted %lu\n", x);
            //struct avl_node *root = *(s->phys_to_virt);
            //printf("Pointer to Tree Root: %p\n", s->phys_to_virt);
            //printf("Pointer to Root Node: %p\n", *(s->phys_to_virt));
            //printf("Current Root Node Value: %lu\n", (root->mapping->paddr));
        }
    }

    for(int i = 0; i < no; i++){
        addr_t res = avl_lookup(s, cop[i]);
        // printf("looking up %lu\n", cop[i]);
        if(verbose)
            printf("Found %lu (%i/%i)\n", res,  i, no);
    }
    if (verbose)
        printf("FOUND TRACE COMPLETE\n");

    clean_tree(*(s->virt_to_phys), false);
    clean_tree(*(s->phys_to_virt), true);
}
