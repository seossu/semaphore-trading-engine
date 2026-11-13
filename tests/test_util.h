#ifndef IG_TEST_UTIL_H
#define IG_TEST_UTIL_H
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "  FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

#define PASS(...)                  \
    do {                           \
        printf("  PASS ");         \
        printf(__VA_ARGS__);       \
        printf("\n");              \
    } while (0)
#endif
