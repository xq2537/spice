/*
   Copyright (C) 2009 Red Hat, Inc.

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

#ifndef _H_SND_WORKER
#define _H_SND_WORKER

#include "vd_interface.h"

void snd_attach_playback(PlaybackInterface *interface);
void snd_detach_playback(PlaybackInterface *interface);

void snd_attach_record(RecordInterface *interface);
void snd_detach_record(RecordInterface *interface);

void snd_set_playback_compression(int on);
int snd_get_playback_compression();

#endif

