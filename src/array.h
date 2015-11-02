#ifndef QUASAR_FDW_ARRAY_H
#define QUASAR_FDW_ARRAY_H

#include <stdlib.h>

typedef struct Array {
    void  *array;
    size_t elementSize;
    size_t used;
    size_t popped;
    size_t size;
} Array;

void array_init(Array *a, size_t elementSize, size_t initialSize);
void *array_get(Array *a, size_t i);
void *array_pop(Array *a);
void array_insert(Array *a, void *element);
void array_free(Array *a);
size_t array_size(Array *a);

typedef struct Array2D {
    Array *rows;
} Array2D;

void array2d_init(Array2D *a, size_t elementSize,
                  size_t initialRows, size_t initialCols);
void array2d_insert(Array2D *a, void *element);
Array *array2d_row(Array2D *a, size_t i);
void *array2d_get(Array2D *a, size_t i, size_t j);
void array2d_linefeed(Array2D *a);
Array *array2d_pop_row(Array2D *a);
void array2d_free(Array2D *a);
size_t array2d_size(Array2D *a);

char *array_get_str(Array *a, size_t i);
char *array2d_get_str(Array2D *a, size_t i, size_t j);

#endif // QUASAR_FDW_ARRAY_H
