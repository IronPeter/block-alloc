//
//  main.cpp
//  fmallo
//
//  Created by Петр on 21.09.12.
//  Copyright (c) 2012 Петр. All rights reserved.
//
#include <pthread.h>
#include <vector>
#include <stdlib.h>
#include <stdio.h>
#include <algorithm>
#include <memory.h>

void *ThreadFunc(void *) {
    void *data[10000];
    for (size_t i = 0; i < 1000; ++i) {
        for (size_t j = 0; j < 10000; ++j) {
            data[j] = malloc(j & 0xff);
            memset(data[j], 0, j & 0xff);
        }
        for (size_t j = 0; j < 10000; ++j) {
            free(data[j]);
        }
    }
    return 0;
};


int main(int argc, const char * argv[]) {
    pthread_t thread[16];
    for (size_t i = 0; i < 16; ++i) {
        pthread_create (&thread[i], NULL, ThreadFunc, 0);
    }
    for (size_t i = 0; i < 16; ++i) {
        pthread_join(thread[i], NULL);
    }
    return 0;
}

