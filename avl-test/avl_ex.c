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
           
typedef unsigned int lpaddr_t;
typedef unsigned int lvaddr_t;
typedef unsigned int addr_t;



typedef enum {
    V_TO_P, P_TO_V
} avl_type;

static struct addr_mapping {
    lpaddr_t vaddr;
    lpaddr_t paddr;
};

static struct avl_node {
    struct addr_mapping *mapping;
    avl_type type;
    struct avl_node *parent; 
    struct avl_node *left;
    struct avl_node *right;
    unsigned short height;
};   

// struct to store the paging status of a process
static struct paging_state {
    struct avl_node *virt_to_phys;
    struct avl_node *phys_to_virt;

};

static void _clean_node_child_ref(struct avl_node *node) {
    // Pre-Conditions
    assert(node != NULL);

    node->left = NULL;
    node->right = NULL;

    // Post-Conditions
    assert(node->left == NULL);
    assert(node->right == NULL);
}

static void avl_update_height(struct avl_node *node){
    if(!node)
        return;
    
    short lh = node->left ? node->left->height : 0;
    short rh = node->right ? node->right->height : 0;
    
    node->height = MAX(lh, rh) + 1;
}


static void avl_rotate_left(struct avl_node *parent, struct avl_node *node) {
    // Pre-Conditions
    assert(node != NULL);
    assert(!parent || parent->right == node || parent->left == node);
    assert(node->right != NULL);
    // assert(node->right->right != NULL && node->right->left == NULL);

    struct avl_node *new_child = node->right;
    struct avl_node *temp = new_child->left;
    new_child->left = node;
    node->right = temp;
    
    if(temp)
        temp->parent = node;
    
    new_child->parent = node->parent;
    node->parent = new_child;
    // _clean_node_child_ref(node);
    avl_update_height(node);
    avl_update_height(new_child);

    if(parent){
        if (parent->left == node) 
            parent->left = new_child;
        else
            parent->right = new_child;   
        avl_update_height(parent);
    }
    
    // Post-Conditions
    assert(!parent || parent->left == new_child || parent->right == new_child);
    assert(new_child->left == node);
}

static void avl_rotate_right(struct avl_node *parent, struct avl_node *node) {
    // Pre-Conditions
    assert(node != NULL);
    assert(!parent || parent->left == node || parent->right == node);
    assert(node->left != NULL);
    // assert(node->left->left != NULL && node->left->right == NULL);

    struct avl_node *new_child = node->left;
    struct avl_node *temp = new_child->right;
    new_child->right = node;
    node->left = temp;
    
    if(temp)
        temp->parent = node;
    
    new_child->parent = node->parent;
    node->parent = new_child;
    // _clean_node_child_ref(node);
    avl_update_height(node);
    avl_update_height(new_child);
    
    if(parent){
        if (parent->left == node) 
            parent->left = new_child;
        else
            parent->right = new_child;  
        avl_update_height(parent);
    }

    // Post-Conditions
    assert(!parent || parent->left == new_child || parent->right == new_child);
    assert(new_child->right == node);
}

static void avl_balance_node(struct avl_node *node) {
    if(!node)
        return;
    
    // get balance for node
    short lh = node->left ? node->left->height : 0;
    short rh = node->right ? node->right->height : 0;
    short balance = lh-rh;
    
    // top points to the top-most node in the subtree
    // (might change after balancing)
    struct avl_node *top = node;
    
    short old_height = node->height;
    avl_update_height(node);
    
    if(balance < -1){
        // handle right subtree too high
        struct avl_node *child = node->right;
        
        // check for right left case
        if(LEFT_HEAVY(child))
            avl_rotate_right(node, child);

        top = node->right;

        // right right case
        avl_rotate_left(node->parent, node);
    } else if(balance > 1){
        // handle left subtree too high
        struct avl_node *child = node->left;
        
        // check for left right case
        if(RIGHT_HEAVY(child))
            avl_rotate_left(node, child);

        top = node->left;

        // left left case
        avl_rotate_right(node->parent, node);
    }
    
    // if height has changed we recursive to parent
    if(old_height != top->height)
        avl_balance_node(top->parent);
}

/*
static void avl_rotate_left_right(struct avl_node *parent, struct avl_node *node) {
    // Pre-Conditions
    assert(node != NULL);
    assert(parent->left == node || parent->right == node);
    assert(node->right != NULL && node->left == NULL);
    assert(node->right->left != NULL && node->right->right == NULL);

    struct avl_node *new_child = node->right;
    new_child->left = node;
    new_child->right = node->right->left;
    _clean_node_child_ref(node);
    if (parent->left == node) 
        parent->left = new_child;
    else
        parent->right = new_child;  

    // Post-Conditions
    assert(parent->left == new_child || parent->right == new_child);
    assert(new_child->left == node && new_child->right != NULL);
}

static void avl_rotate_right_left(struct avl_node *parent, struct avl_node *node) {
    // Pre-Conditions
    assert(node != NULL);
    assert(parent->left == node || parent->right == node);
    assert(node->left != NULL && node->right == NULL);
    assert(node->left->left == NULL && node->left->right != NULL);

    struct avl_node *new_child = node->left;
    new_child->left = node->left->right;
    new_child->right = node;
    _clean_node_child_ref(node);
    if (parent->left == node) 
        parent->left = new_child;
    else
        parent->right = new_child;  

    // Post-Conditions
    assert(parent->left == new_child || parent->right == new_child);
    assert(new_child->left != NULL && new_child->right == node);
}
*/


/**
 * Maybe better with iterative loop instead of recursion?
 */
static struct avl_node* avl_lookup_aux(struct avl_node *node, addr_t addr){
    addr_t key = node->type == V_TO_P ? node->mapping->vaddr : node->mapping->paddr;
    if(!node)
        return NULL;
    else if(addr < key)
        return avl_lookup_aux(node->right, addr);
    else if(addr > key)
        return avl_lookup_aux(node->left, addr);
    else
        // addr == node->key
        return node;
}

addr_t avl_lookup(struct paging_state *s, addr_t addr){
    struct avl_node *res = avl_lookup_aux(s->phys_to_virt, addr);
    return res->mapping->vaddr;
}

// Why pointer to pointer? Complex semantics
static void insert_node(struct avl_node **parent, struct avl_node *child){
    if(!(*parent)){
        *parent = child;
        avl_balance_node(child->parent);
    } else {
        addr_t p_key = child->type == V_TO_P
            ? (*parent)->mapping->vaddr
            : (*parent)->mapping->paddr;

        addr_t n_key = child->type == V_TO_P
            ? child->mapping->vaddr
            : child->mapping->paddr;
        
        child->parent = *parent;
        if(p_key > n_key){
            insert_node(&(*parent)->left, child);
        } else if(p_key < n_key) {
            insert_node(&(*parent)->right, child);
        }
        else
            // handle value already in tree
            ;
    }
}

// Alternatve Node Insertion Function.
// I presume Traverse is used to find the parent and then the child gets inserted once
// its found.
// So no need for traversing within insert_node?
// Wrote this to propose Simpler Semantics. 
static void _insert_node(struct avl_node *parent, struct avl_node *child) {
    // Pre-Condition
    assert(parent != NULL && child != NULL);
    assert(parent->type == child->type);
    
    addr_t p_key = child->type == V_TO_P
        ? parent->mapping->vaddr
        : parent->mapping->paddr;

    addr_t n_key = child->type == V_TO_P
        ? child->mapping->vaddr
        : child->mapping->paddr;
   
    // OPTIONAL 
    child->parent = parent;
    
    if (p_key > n_key)
        parent->left = child;
    else
        parent->right = child;

    // Post-Condition
    assert(parent->left == child || parent->right == child);
    assert(child->left == NULL && child->right == NULL);
}

// There is the edge case where removing a mapping will result in another
// node being the root node instead. 
// So the function should return an avl_node pointer to inform of the newest root node.
static struct avl_node* remove_mapping(struct paging_state *s, addr_t addr){
    ; //TODO
}

int _print_t(struct avl_node *tree, int is_left, int offset, int depth, char s[20][255*4])
{
    char b[15];
    int width = 12;

    if (!tree) return 0;

    struct addr_mapping *m = tree->mapping;
    addr_t key = tree->type == P_TO_V ? m->paddr : m->vaddr;
    addr_t val = tree->type == P_TO_V ? m->vaddr : m->paddr;
    sprintf(b, "%02x -> %02x (%i)", key, val, tree->height);

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

void print_t(struct avl_node *tree)
{
    char s[20][255*4];
    for (int i = 0; i < 20; i++)
        sprintf(s[i], "%80s", " ");

    _print_t(tree, 0, 0, 0, s);

    for (int i = 0; i < 20; i++)
        printf("%s\n", s[i]);
}

struct addr_mapping create_mapping(lpaddr_t paddr, lvaddr_t vaddr){
    struct addr_mapping m;
    m.paddr = paddr;
    m.vaddr = vaddr;
    return m;
}

struct avl_node create_node(struct addr_mapping *m, avl_type type){
    struct avl_node n;
    n.mapping = m;
    n.type = type;
    n.parent = NULL;
    n.left = NULL;
    n.right = NULL;
    n.height = 1;
    return n;
}

void insert_mapping(struct paging_state *s, lvaddr_t vaddr, lpaddr_t paddr){
    struct addr_mapping *m =
        (struct addr_mapping *)malloc(sizeof(struct addr_mapping));
    *m = create_mapping(vaddr, paddr);

    struct avl_node *n_p2v =
        (struct avl_node *)malloc(sizeof(struct avl_node));
    *n_p2v = create_node(m, P_TO_V);

    struct avl_node *n_v2p =
        (struct avl_node *)malloc(sizeof(struct avl_node));
    *n_v2p = create_node(m, V_TO_P);

    struct avl_node *new_root;

    insert_node(&s->phys_to_virt, n_p2v);
    while((new_root = s->phys_to_virt->parent))
        s->phys_to_virt = new_root;
    
    insert_node(&s->virt_to_phys, n_v2p);
    while((new_root = s->virt_to_phys->parent))
        s->virt_to_phys = new_root;
}

int lookup_array(int *i_arr, int elm){
    if(elm == i_arr[0])
        return elm;
    else
        return lookup_array(&i_arr[1], elm);
}


int main(int argc, char **argv) {
    struct paging_state s;

    s.phys_to_virt = NULL;
    s.virt_to_phys = NULL;

    int no = atoi(argv[1]);
    int cop[no];
    int x;

    for(int i = 0; i < no; i++){
        printf("inserted %d\n", i);
        scanf("%d", &x);
        cop[i] = x;
        insert_mapping(&s, x, x);
    }

    printf("phys to virt:\n");
    print_t(s.phys_to_virt);

    for(int i = 0; i < no; i++){
        printf("looking up %d\n", cop[i]);
        avl_lookup(&s, cop[i]);
    }

    //
    // printf("phys to virt:\n");
    // print_t(s.phys_to_virt);
    //
    // printf("virt to phys:\n");
    // print_t(s.virt_to_phys);

    
}
