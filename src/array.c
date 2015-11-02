#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "array.h"


void array_init(Array *a, size_t elementSize, size_t initialSize) {
    assert(initialSize > 0);
    a->array = malloc(initialSize * elementSize);
    a->elementSize = elementSize;
    a->used = a->popped = 0;
    a->size = initialSize;
}

void *array_pop(Array *a) {
    void * el = array_get(a, 0);
    ++a->popped;
    return el;
}

void *array_get(Array *a, size_t i) {
    assert(i + a->popped < a->used);
    return (void*) ((char*) a->array) + a->elementSize * (i + a->popped);
}

size_t array_size(Array *a) {
    return a->used - a->popped;
}

void array_insert(Array *a, void *element) {
    if (a->used == a->size) {
        a->size *= 2;
        a->array = realloc(a->array, a->size * a->elementSize);
    }

    int offset = a->elementSize * (a->used++);
    memcpy(((char *) a->array) + offset, element, a->elementSize);
}

void array_free(Array *a) {
    free(a->array);
    a->array = NULL;
    a->used = a->size = a->popped = 0;
}

void array2d_new_row(Array2D *a, size_t elementSize, size_t initialCols) {
    Array * row = malloc(sizeof(Array));
    array_init(row, elementSize, initialCols);
    array_insert(a->rows, row);
}
Array *array2d_last_row(Array2D *a) {
    return array2d_row(a, array2d_size(a) - 1);
}
void array2d_init(Array2D *a, size_t elementSize,
                  size_t initialRows, size_t initialCols) {
    a->rows = malloc(sizeof(Array));
    array_init(a->rows, sizeof(Array), initialRows);
    array2d_new_row(a, elementSize, initialCols);
}
Array *array2d_row(Array2D *a, size_t i) {
    return array_get(a->rows, i);
}
void *array2d_get(Array2D *a, size_t i, size_t j) {
    return array_get(array2d_row(a, i), j);
}
size_t array2d_size(Array2D *a) {
    return array_size(a->rows);
}
Array *array2d_pop_row(Array2D *a) {
    return array_pop(a->rows);
}
void array2d_insert(Array2D *a, void *element) {
    array_insert(array2d_last_row(a), element);
}
void array2d_linefeed(Array2D *a) {
    Array *last_row = array2d_last_row(a);
    array2d_new_row(a, last_row->elementSize, last_row->used);
}

void array2d_free(Array2D *a) {
    for (size_t i = 0; i < array2d_size(a); ++i) {
        array_free(array_get(a->rows, i));
    }
    array_free(a->rows);
}


char *array_get_str(Array *a, size_t i) {
    return *(char**)array_get(a, i);
}
char *array2d_get_str(Array2D *a, size_t i, size_t j) {
    return array_get_str(array2d_row(a, i), j);
}
