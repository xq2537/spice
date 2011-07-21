/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2011 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Taken from xserver os/backtrace.c:
 * Copyright 2008 Red Hat, Inc.
 */

#include "config.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "spice_common.h"

#define GSTACK_PATH "/usr/bin/gstack"

#if HAVE_EXECINFO_H
#include <execinfo.h>

static void spice_backtrace_backtrace(void)
{
    void *array[100];
    int size;

    size = backtrace(array, sizeof(array)/sizeof(array[0]));
    backtrace_symbols_fd(array, size, STDERR_FILENO);
}
#else
static void spice_backtrace_backtrace(void)
{
}
#endif

static int spice_backtrace_gstack(void)
{
    pid_t kidpid;
    int pipefd[2];

    if (pipe(pipefd) != 0) {
        return -1;
    }

    kidpid = fork();

    if (kidpid == -1) {
        /* ERROR */
        return -1;
    } else if (kidpid == 0) {
        /* CHILD */
        char parent[16];

        seteuid(0);
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        dup2(pipefd[1],STDOUT_FILENO);
        close(STDERR_FILENO);

        snprintf(parent, sizeof(parent), "%d", getppid());
        execle(GSTACK_PATH, "gstack", parent, NULL, NULL);
        exit(1);
    } else {
        /* PARENT */
        char btline[256];
        int kidstat;
        int bytesread;
        int done = 0;

        close(pipefd[1]);

        while (!done) {
            bytesread = read(pipefd[0], btline, sizeof(btline) - 1);

            if (bytesread > 0) {
                btline[bytesread] = 0;
                fprintf(stderr, "%s", btline);
            }
            else if ((bytesread == 0) ||
                     ((errno != EINTR) && (errno != EAGAIN))) {
                done = 1;
            }
        }
        close(pipefd[0]);
        waitpid(kidpid, &kidstat, 0);
        if (kidstat != 0)
            return -1;
    }
    return 0;
}

void spice_backtrace() {
    if (!access(GSTACK_PATH, X_OK)) {
        spice_backtrace_gstack();
    } else {
        spice_backtrace_backtrace();
    }
}
