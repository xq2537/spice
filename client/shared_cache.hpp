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

#ifndef _H_SHARED_CACHE
#define _H_SHARED_CACHE

#include "utils.h"
#include "threads.h"

/*class SharedCache::Treat {
    T* get(T*);
    void release(T*);
    const char* name();
};*/

template <class T, class Treat, int HASH_SIZE, class Base = EmptyBase>
class SharedCache : public Base {
public:
    SharedCache()
        : _aborting (false)
    {
        memset(_hash, 0, sizeof(_hash));
    }

    ~SharedCache()
    {
        clear();
    }

    void add(uint64_t id, T* data)
    {
        Lock lock(_lock);
        Item** item = &_hash[key(id)];

        while (*item) {
            if ((*item)->id == id) {
                (*item)->refs++;
                return;
            }
            item = &(*item)->next;
        }
        *item = new Item(id, data);
        _new_item_cond.notify_all();
    }

    T* get(uint64_t id)
    {
        Lock lock(_lock);
        Item* item = _hash[key(id)];

        for (;;) {
            if (!item) {
                if (_aborting) {
                    THROW("%s aborting", Treat::name());
                }
                _new_item_cond.wait(lock);
                item = _hash[key(id)];
                continue;
            }

            if (item->id != id) {
                item = item->next;
                continue;
            }

            return Treat::get(item->data);
        }
    }

    void remove(uint64_t id)
    {
        Lock lock(_lock);
        Item** item = &_hash[key(id)];

        while (*item) {
            if ((*item)->id == id) {
                if (!--(*item)->refs) {
                    Item *rm_item = *item;
                    *item = rm_item->next;
                    delete rm_item;
                }
                return;
            }
            item = &(*item)->next;
        }
        THROW("%s id %lu, not found", Treat::name(), id);
    }

    void clear()
    {
        Lock lock(_lock);
        for (int i = 0; i < HASH_SIZE; i++) {
            while (_hash[i]) {
                Item *item = _hash[i];
                _hash[i] = item->next;
                delete item;
            }
        }
    }

    void abort()
    {
        Lock lock(_lock);
        _aborting = true;
        _new_item_cond.notify_all();
    }

private:
    inline uint32_t key(uint64_t id) {return uint32_t(id) % HASH_SIZE;}

private:
    class Item {
    public:
        Item(uint64_t in_id, T* data)
            : id (in_id)
            , refs (1)
            , next (NULL)
            , data (Treat::get(data)) {}

        ~Item()
        {
            Treat::release(data);
        }

        uint64_t id;
        int refs;
        Item* next;
        T* data;
    };

    Item* _hash[HASH_SIZE];
    Mutex _lock;
    Condition _new_item_cond;
    bool _aborting;
};

#endif

