#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
 
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
    signed short balance;
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

static void avl_rotate_left(struct avl_node *parent, struct avl_node *node) {
    // Pre-Conditions
    assert(node != NULL);
    assert(parent->right == node || parent->left == node);
    assert(node->right != NULL && node->left == NULL);
    assert(node->right->right != NULL && node->right->left == NULL);

    struct avl_node *new_child = node->right;
    new_child->left = node;
    _clean_node_child_ref(node); 
    if (parent->left == node) 
        parent->left = new_child;
    else
        parent->right = new_child;   
 
    // Post-Conditions
    assert(parent->left == new_child || parent->right == new_child);
    assert(new_child->left == node && new_child->right != NULL);
}

static void avl_rotate_right(struct avl_node *parent, struct avl_node *node) {
    // Pre-Conditions
    assert(node != NULL);
    assert(parent->left == node || parent->right == node);
    assert(node->left != NULL && node->right == NULL);
    assert(node->left->left != NULL && node->left->right == NULL);

    struct avl_node *new_child = node->left;
    new_child->right = node;
    _clean_node_child_ref(node);
    if (parent->left == node) 
        parent->left = new_child;
    else
        parent->right = new_child;  
 
    // Post-Conditions
    assert(parent->left == new_child || parent->right == new_child);
    assert(new_child->right == node && new_child->left != NULL);
}

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

static void avl_make_balanced(struct avl_node *node) {
    ; // TODO

}

/**
 * Maybe better with iterative loop instead of recursion?
 */
static struct avl_node* avl_traverse(struct avl_node *node, addr_t addr){
    addr_t key = node->type == V_TO_P ? node->mapping->vaddr : node->mapping->paddr;
    if(!node)
        return NULL;
    else if(addr < key)
        return avl_traverse(node->right, addr);
    else if(addr > key)
        return avl_traverse(node->left, addr);
    else
        // addr == node->key
        return node;
}

static void insert_node(struct avl_node **parent, struct avl_node *child){
    if(!(*parent))
        *parent = child;
    else {
        addr_t p_key = child->type == V_TO_P
                     ? (*parent)->mapping->vaddr
                     : (*parent)->mapping->paddr;

        addr_t n_key = child->type == V_TO_P
                     ? child->mapping->vaddr
                     : child->mapping->paddr;
        ;
        child->parent = *parent;
        if(p_key > n_key)
            insert_node(&(*parent)->left, child);
        else if(p_key < n_key)
            insert_node(&(*parent)->right, child);
        else
            // handle value already in tree
            return;
    }
}

// There is the edge case where removing a mapping will result in another
// node being the root node instead. 
static struct avl_node* remove_mapping(struct paging_state *s, addr_t addr){
    ; //TODO
}

int _print_t(struct avl_node *tree, int is_left, int offset, int depth, char s[20][255])
{
    char b[20];
    int width = 8;

    if (!tree) return 0;
    
    struct addr_mapping *m = tree->mapping;
    addr_t key = tree->type == P_TO_V ? m->paddr : m->vaddr;
    addr_t val = tree->type == P_TO_V ? m->vaddr : m->paddr;
    sprintf(b, "%02x -> %02x", key, val);

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
    char s[20][255];
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
    n.balance = 0;
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
    
    insert_node(&s->phys_to_virt, n_p2v);
    insert_node(&s->virt_to_phys, n_v2p);
}

int main() {
    struct paging_state s;
    
    s.phys_to_virt = NULL;
    s.virt_to_phys = NULL;
    
    // phys to virt mappings
    insert_mapping(&s, 0x10, 0x00);
    insert_mapping(&s, 0xA2, 0x04);
    insert_mapping(&s, 0x04, 0x08);
    insert_mapping(&s, 0x14, 0x0C);
    insert_mapping(&s, 0xB4, 0x10);
    insert_mapping(&s, 0x28, 0x14);
    insert_mapping(&s, 0x88, 0x18);
    
    printf("phys to virt:\n");
    print_t(s.phys_to_virt);
    
    printf("virt to phys:\n");
    print_t(s.virt_to_phys);
    
}
