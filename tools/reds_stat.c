/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "reds_stat.h"

#define TAB_LEN 4
#define VALUE_TABS 7
#define INVALID_STAT_REF (~(uint32_t)0)

static RedsStat *reds_stat = NULL;
static uint64_t *values = NULL;

void print_stat_tree(int32_t node_index, int depth)
{
    StatNode *node = &reds_stat->nodes[node_index];
    int i;

    if ((node->flags & STAT_NODE_MASK_SHOW) == STAT_NODE_MASK_SHOW) {
        printf("%*s%s", depth * TAB_LEN, "", node->name);
        if (node->flags & STAT_NODE_FLAG_VALUE) {
            printf(":%*s%llu (%llu)\n", (VALUE_TABS - depth) * TAB_LEN - strlen(node->name) - 1, "",
                   node->value, node->value - values[node_index]);
            values[node_index] = node->value;
        } else {
            printf("\n");
            if (node->first_child_index != INVALID_STAT_REF) {
                print_stat_tree(node->first_child_index, depth + 1);
            }
        }
    }
    if (node->next_sibling_index != INVALID_STAT_REF) {
        print_stat_tree(node->next_sibling_index, depth);
    }
}

int main(int argc, char **argv)
{
    char *shm_name;
    pid_t kvm_pid;
    uint64_t *val;
    uint32_t num_of_nodes = 0;
    size_t shm_size;
    size_t shm_old_size;
    int shm_name_len;
    int ret = -1;
    int fd;

    if (argc != 2 || !(kvm_pid = atoi(argv[1]))) {
        printf("usage: reds_stat [qemu_pid] (e.g. `pgrep qemu`)\n");
        return -1;
    }
    shm_name_len = strlen(REDS_STAT_SHM_NAME) + strlen(argv[1]);
    if (!(shm_name = (char *)malloc(shm_name_len))) {
        perror("malloc");
        return -1;
    }
    snprintf(shm_name, shm_name_len, REDS_STAT_SHM_NAME, kvm_pid);
    if ((fd = shm_open(shm_name, O_RDONLY, 0444)) == -1) {
        perror("shm_open");
        free(shm_name);
        return -1;
    }
    shm_size = sizeof(RedsStat);
    reds_stat = mmap(NULL, shm_size, PROT_READ, MAP_SHARED, fd, 0);
    if (reds_stat == (RedsStat *)MAP_FAILED) {
        perror("mmap");
        goto error1;
    }
    if (reds_stat->magic != REDS_STAT_MAGIC) {
        printf("bad magic %u\n", reds_stat->magic);
        goto error2;
    }
    if (reds_stat->version != REDS_STAT_VERSION) {
        printf("bad version %u\n", reds_stat->version);
        goto error2;
    }
    while (1) {
        system("clear");
        printf("spice statistics\n\n");
        if (num_of_nodes != reds_stat->num_of_nodes) {
            num_of_nodes = reds_stat->num_of_nodes;
            shm_old_size = shm_size;
            shm_size = sizeof(RedsStat) + num_of_nodes * sizeof(StatNode);
            reds_stat = mremap(reds_stat, shm_old_size, shm_size, MREMAP_MAYMOVE);
            if (reds_stat == (RedsStat *)MAP_FAILED) {
                perror("mremap");
                goto error3;
            }
            values = (uint64_t *)realloc(values, num_of_nodes * sizeof(uint64_t));
            if (values == NULL) {
                perror("realloc");
                goto error3;
            }
            memset(values, 0, num_of_nodes * sizeof(uint64_t));
        }
        print_stat_tree(reds_stat->root_index, 0);
        sleep(1);
    }
    ret = 0;

error3:
    free(values);
error2:
    munmap(reds_stat, shm_size);
error1:
    shm_unlink(shm_name);
    free(shm_name);
    return ret;
}

