/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Drives the connection pill mapping (muse_pill.c) for test_muse_pill.py.
 *   label N   prints the pill label for N (an int cast to muse_pill_t)
 *   dot N     prints the pill dot for N as %06X
 *   from H L  prints "<label> <dot>" for muse_pill_from_link(H, L != 0) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_chat.h"
#include "muse_pill.h"

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "label")) {
        printf("%s\n", muse_pill_label((muse_pill_t)atoi(argv[2])));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "dot")) {
        printf("%06X\n", (unsigned)muse_pill_dot((muse_pill_t)atoi(argv[2])));
        return 0;
    }
    if (argc > 3 && !strcmp(argv[1], "from")) {
        muse_pill_t pill =
            muse_pill_from_link((muse_hatch_state_t)atoi(argv[2]), atoi(argv[3]) != 0);
        printf("%s %06X\n", muse_pill_label(pill), (unsigned)muse_pill_dot(pill));
        return 0;
    }
    fprintf(stderr, "usage: %s label N | dot N | from H L\n", argv[0]);
    return 2;
}
