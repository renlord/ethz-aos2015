#include <stdio.h>
#include <stdlib.h>
#include <assert.h>


long unsigned lookup_array(long unsigned *i_arr, long unsigned elm){
    if(elm == i_arr[0])
        return elm;
    else
        return lookup_array(&i_arr[1], elm);
}


int main(int argc, char **argv) {
    int verbose = atoi(argv[1]);
    int no = atoi(argv[2]);
    
    long unsigned *test = malloc(no*sizeof(long unsigned));
    
    for(int i = 0; i < no; i++){
        scanf("%lu", test+i);
        if(verbose)
            printf("inserted %lu\n", test[i]);
    }
    
    for(int i = 0; i < no; i++){
        long unsigned res = lookup_array(test, test[i]);
        if(verbose)
            printf("Found %lu (%i/%i)\n", res,  i, no);
    }
    
    free(test);
}
