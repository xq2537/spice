/*
   Copyright (C) 2009 Red Hat, Inc.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in
         the documentation and/or other materials provided with the
         distribution.
       * Neither the name of the copyright holder nor the names of its
         contributors may be used to endorse or promote products derived
         from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _H_VDI_DEV
#define _H_VDI_DEV

#include "ipc_ring.h"

#ifdef __GNUC__
#ifdef __i386__
#define mb() __asm__ __volatile__ ("lock; addl $0,0(%%esp)": : :"memory")
#else
//mfence
#define mb() __asm__ __volatile__ ("lock; addl $0,0(%%rsp)": : :"memory")
#endif
#define ATTR_PACKED __attribute__ ((__packed__))
#else
#pragma pack(push)
#pragma pack(1)
#define ATTR_PACKED
#define mb() __asm {lock add [esp], 0}
#endif

#define REDHAT_PCI_VENDOR_ID 0x1b36

#define VDI_PORT_DEVICE_ID 0x0105
#define VDI_PORT_REVISION 0x01

#define VDI_PORT_INTERRUPT (1 << 0)

#define VDI_PORT_MAGIC (*(UINT32*)"VDIP")

typedef struct ATTR_PACKED VDIPortPacket {
    UINT32 gen;
    UINT32 size;
    UINT8 data[512 - 2 * sizeof(UINT32)];
} VDIPortPacket;

RING_DECLARE(VDIPortRing, VDIPortPacket, 32);

enum {
    VDI_PORT_IO_RANGE_INDEX,
    VDI_PORT_RAM_RANGE_INDEX,
};

enum {
    VDI_PORT_IO_CONNECTION,
    VDI_PORT_IO_NOTIFY = 4,
    VDI_PORT_IO_UPDATE_IRQ = 8,

    VDI_PORT_IO_RANGE_SIZE = 12
};

typedef struct ATTR_PACKED VDIPortRam {
    UINT32 magic;
    UINT32 generation;
    UINT32 int_pending;
    UINT32 int_mask;
    VDIPortRing input;
    VDIPortRing output;
    UINT32 reserv[32];
} VDIPortRam;

#ifndef __GNUC__
#pragma pack(pop)
#endif

#undef ATTR_PACKED

#endif
