#include <stdio.h>
#include "sfmm.h"


// This main file is what you can use to test out the functions given in sfmm.c
// Below is leftover code from me testing while doing the assignment
// I leave it here to show my thought process
int main(int argc, char const *argv[]) {
    // double* ptr = sf_malloc(sizeof(double));

    // *ptr = 320320320e-320;

    // printf("%f\n", *ptr);

    // sf_free(ptr);
    // Set obf/magic to 0 to see if error is due to that
    // sf_set_magic(0x0);

    sf_errno = 0;
	// We want to allocate up to exactly four pages, so there has to be space
	// for the header and the link pointers.
	void *x = sf_malloc(16316);
    (void) x;

    return EXIT_SUCCESS;
}
