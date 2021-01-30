
#pragma once

#include <cstdio>

namespace bjit
{
    // This uses the mix13 constants (also used by splitmix64) from
    // https://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html
    static uint64_t hash64(uint64_t x)
    {
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9LLU;
        x ^= x >> 27; x *= 0x94d049bb133111ebLLU;
        x ^= x >> 31;
        return x;
    }

    // This does 32-bits at a time using hash64 with the upper bytes
    // set to the remaining length of the string.
    //
    // It's basically a variant of the sponge-construction, except we
    // xor the length into the capacity on every round.
    static uint64_t stringHash64(const uint8_t * bytes, uint32_t nBytes)
    {
        uint64_t x = 0;
        uint64_t seed = ((uint64_t)nBytes) << 32;
        while(nBytes >= 4)
        {
            x ^= (*(uint32_t*)bytes);
            x ^= seed;
            x = hash64(x);

            bytes += 4; nBytes -= 4;
        }

        switch(nBytes)
        {
            case 3: x += bytes[2] << 16;
            case 2: x += bytes[1] << 8;
            case 1: x += bytes[0];
                x ^= seed;
                x = hash64(x);
            default:
                break;
        }
        
        return x;
    }

    // This is a closed hashtable that stores a set of items
    // and which allows searching by any key for which the
    // Item-type provides methods getHash(Key) and isEqual(Key),
    // which means it can be used as either a set or a map.
    template <typename Item>
    struct HashTable
    {
        // minimum number of slots to use
        static const unsigned minSlots = 4;

        // resize when less than 1/freeFactor slots are free
        // this should normally be between 2 and 4
        //
        static const unsigned freeFactor = 3;
    
        // this implements visitor pattern
        // calls fn(key, value) for each non-null pair
        template <class Visitor>
        void foreach(Visitor && fn)
        {
            for(unsigned i = 0; i < slots.size(); ++i)
            {
                Slot & s = slots[i];
                if(slotInUse == (s.hash & 0x3))
                {
                    fn(s.key, s.value);
                }
            }
        }

        unsigned size() { return nUsed; }

        HashTable(unsigned reserve = 0)
        {
            // calculate next power of two up from size
            unsigned wantSize = minSlots;
            while(wantSize < reserve) wantSize <<= 1;

            resize(wantSize);
        }

        // return existing item matching key or null
        template <typename Key>
        Item * find(const Key & k)
        {
            Slot & s = internalFind(k, false);
            if((s.hash & 0x3) != slotInUse) return 0;
            return &s.item;
        }

        // remove existing item matching key if any
        template <typename Key>
        void remove(const Key & k)
        {
            Slot & s = internalFind(k, false);
            if((s.hash & 0x3) == slotInUse)
            {
                s.hash = slotRemoved | (s.hash & ~(uint64_t)0x3);
                s.item = std::move(Item());
                --nUsed;
            }
        }

        // add a new item - any existing matching key is replaced
        void insert(Item & i)
        {
            Slot & s = internalFind(i, true);
            s.item = std::move(i);

            // check for resize, multiply out the divides from:
            //   (nSlots - nUsed) / nSlots < 1 / freeFactor
            if((slots.size() - (++nUsed)) * freeFactor < slots.size())
            {
                resize(slots.size() << 1);
            }
        }

        // if Item provides a converting constructor, allow direct call
        template <typename T>
        void insert(T & t) { Item tmp(t); insert(tmp); }

        // clear the whole table
        void clear()
        {
            for(unsigned i = 0; i < slots.size(); ++i)
            {
                Slot & s = slots[i];
                s.hash = slotFree;
                s.item = std::move(Item());
            }
        }

        // explicit rehash is useful in some situations
        // to clear lazily deleted junk that leads to long probes
        //
        // this can also optionally attempt to compact the table
        //
        void rehash(bool compact = false)
        {
            unsigned wantSlots = slots.size();
            if(compact)
            {
                // reserve at least minSlots before resize
                unsigned needSlots = nUsed + minSlots;

                // can we halve the size?
                while(wantSlots > minSlots)
                {
                    // next candidate size
                    unsigned halfSlots = wantSlots >> 1;

                    // would this cause a resize up
                    if(halfSlots
                        > (halfSlots - needSlots) * freeFactor) break;

                    // accept the smaller size and iterate
                    wantSlots = halfSlots;
                }
            }

            // do the actual rehash
            resize(wantSlots);
        }

    private:
        // these are stored in low 2-bits of Slot's hash
        enum { slotFree, slotInUse, slotRemoved };
        struct Slot
        {
            Item        item;
            uint64_t    hash;

            // default to both null-pointers
            Slot() : hash(slotFree) {}
        };

        unsigned    nUsed;  // for resize control

        // use std::vector for memory management
        std::vector<Slot>   slots;

        // the ultimate hash probe of death:
        //   - use a 2nd hash (upper 32 bits) to seed the probe
        //   - force the 2nd hash to be odd (all slots for pow2)
        //   - then use quadratic probe order on that
        //   - terrible for cache, but shouldn't cluster
        uint32_t probe(uint64_t hash, unsigned j)
        {
            return (hash + ((hash>>32)|1)*((j+j*j)/2)) & (slots.size() - 1);
        }

        // find a slot for a given key, or a free slot to insert into
        template <typename Key>
        Slot & internalFind(const Key & k, bool doInsert)
        {
            uint64_t hash = Item::getHash(k);

            // probe loop
            for(int j = 0; j < slots.size(); ++j)
            {
                unsigned i = probe(hash, j);

                Slot & s = slots[i];

                // is this a free slot or a slot for this key
                if((slotFree == (s.hash & 0x3))
                || (s.hash>>2 == hash>>2 && s.item.isEqual(k)))
                {
                    if(doInsert)
                    {
                        // mark the slot as in use and set hash
                        s.hash = slotInUse | (hash & ~(uint64_t)0x3);
                    }
                    return s;
                }

                // if this is a removed slot, then we need to
                // do a further probe
                if(slotRemoved == (s.hash & 0x3))
                {
                    while(++j < slots.size())
                    {
                        i = probe(hash, j);

                        Slot & ss = slots[i];

                        // if we find free slot, then key not found
                        // and we can reuse the removed slot
                        if(slotFree == (ss.hash & 0x3)) break;

                        // if we find legit match, return this slot
                        if(ss.hash>>2 == hash>>2 && ss.item.isEqual(k))
                        {
                            if(doInsert)
                            {
                                // mark the slot as in use and set hash
                                ss.hash = slotInUse | (hash & ~(uint64_t)0x3);
                            }
                            return ss;
                        }
                    }

                    // didn't find a legit match
                    // so reuse the first candidate
                    if(doInsert)
                    {
                        // mark the slot as in use and set hash
                        s.hash = slotInUse | (hash & ~(uint64_t)0x3);
                    }
                    return s;
                }
            }

            fprintf(stderr, "hashtable warning: probe failed\n");

            // if we are here then something is wrong with
            // our probing function.. but resize to play safe
            rehash(slots.size() << 1);

            // recursively try again, should never happen
            return internalFind(k, doInsert);
        }

        void resize(unsigned newSize)
        {
            // create a new vector with new size
            std::vector<Slot> tmp(newSize);

            // swap with the existing slots
            slots.swap(tmp);

            // reset load factor, this gets recalculated
            nUsed = 0;

            // loop the old table to rehash
            for(unsigned i = 0; i < tmp.size(); ++i)
            {
                Slot & s = tmp[i];
                if(slotInUse == (s.hash&0x3))
                {
                    insert(s.item);
                }
            }
        }
    };
};