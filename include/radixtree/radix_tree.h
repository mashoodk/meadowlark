/*
 *  (c) Copyright 2016-2017, 2021 Hewlett Packard Enterprise Development Company LP.
 *
 *  This software is available to you under a choice of one of two
 *  licenses. You may choose to be licensed under the terms of the
 *  GNU Lesser General Public License Version 3, or (at your option)
 *  later with exceptions included below, or under the terms of the
 *  MIT license (Expat) available in COPYING file in the source tree.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  As an exception, the copyright holders of this Library grant you permission
 *  to (i) compile an Application with the Library, and (ii) distribute the
 *  Application containing code generated by the Library and added to the
 *  Application during this compilation process under terms of your choice,
 *  provided you also meet the terms and conditions of the Application license.
 *
 */

#ifndef RADIX_TREE_H
#define RADIX_TREE_H

#include <functional>
#include <stack>
#include <utility> // pair

#include "nvmm/global_ptr.h"
#include "nvmm/memory_manager.h"
#include "nvmm/heap.h"

#include "radixtree/common.h"

#include "radix_tree_metrics.h"

namespace radixtree {

using Gptr = nvmm::GlobalPtr;
using Mmgr = nvmm::MemoryManager;
using Heap = nvmm::Heap;

typedef enum {

    FIND_OR_CREATE = 0,
    UPDATE = 1

}UpdateFlags;


class RadixTree {

public:
    struct Iter {
        // info on the range query
        std::string begin_key;
        bool begin_key_inclusive;
        bool begin_key_open;

        std::string end_key;
        bool end_key_inclusive;
        bool end_key_open;

        // current node
        Gptr node; // == 0: range scan is done, no more valid keys
        uint64_t next_pos; // the next value or child ptr to visit; 0: value; >0: pos-1 is the index of the key in the child array

        // current key and value
        std::string key; // current key
        TagGptr value; // current value

        // traversal history
        std::stack<std::pair<Gptr, uint64_t>> path;
    };

    static const size_t MAX_KEY_LEN = 40;

    // NOTE:
    // - an open key (inf) == '\0' and exclusive
    // - a regular key ("\0") == '\0' and inclusive
    // - ['\0','\0'] => '\0'
    // - ['\0','\0') => ['\0', +inf)
    // - ('\0','\0'] => (-inf, '\0']
    // - ('\0','\0') => (-inf, +inf)
    static constexpr char const * OPEN_BOUNDARY_KEY = "\0";
    static const size_t OPEN_BOUNDARY_KEY_SIZE = 1;

    // a radix tree is uniquely identified by the memory manager instance, the heap id, and the root pointer 
    // when Root=0, create a new radix tree with the provied memory manager and heap; get_root() will return the root pointer
    // when Root!=0, open an existing radix tree whose root pointer is Root, with the provied memory manager and heap
    RadixTree(Mmgr *Mmgr, Heap *Heap, RadixTreeMetrics* Metrics, Gptr Root=0);
    virtual ~RadixTree();

    // returns the root ptr of the radix tree
    Gptr get_root();

    // returns 0 if the key does not exist (insert)
    // returns old value if the key exists (update)
    TagGptr put(const char * key, const size_t key_size, Gptr value, UpdateFlags update);

    // returns 0 if not found
    TagGptr get(const char * key, const size_t key_size);

    // returns 0 if not found
    // returns old value if any; caller owns it
    TagGptr destroy (const char * key, const size_t key_size);

    void list(std::function<void(const char*, const size_t, Gptr)> f);

    void structure();

    // for scan
    // returns -1 if there is no key in range
    int scan(Iter &iter,
             char *key, size_t &key_size, TagGptr &value,
             const char * begin_key, const size_t begin_key_size, const bool begin_key_inclusive,
             const char * end_key, const size_t end_key_size, const bool end_key_inclusive);

    // return -1 if there is no next key
    int get_next(Iter &iter,
                 char * key, size_t& key_size, TagGptr &value);


    /*
      for consistent DRAM caching

      NOTE:
      - when we say "key did not exist", we mean the key NODE did not exist
      - when we say "key was deleted", we mean the key NODE still exists but the value pointer was
      set to null with a valid version number
      - old_value is the previous value pointer in the key node before put or destroy, or null with
      version 0 if the key node did not exist
    */

    // return <key ptr, new value ptr> for both insert and update
    // old value ptr is returned through old_value
    // old value ptr could be null with a valid version if the key was deleted or the key did not exist
    // the key ptr will always be valid
    std::pair<Gptr, TagGptr> putC(const char * key, const size_t key_size, Gptr value, TagGptr &old_value);

    // return new value ptr for both insert and update
    // old value ptr is returned through old_value
    // old value ptr could be null with a valid version if the key was deleted
    TagGptr putC(Gptr const key_ptr, Gptr value, TagGptr &old_value);

    // return both key ptr and value ptr
    // the value ptr will be null with a valid version if the key was deleted or the key did not exist
    // the key ptr will be null if the key did not exist
    std::pair<Gptr, TagGptr> getC(const char * key, const size_t key_size);

    // return both key ptr and value ptr
    // old value ptr could be null with a valid version if the key was deleted
    TagGptr getC(Gptr const key_ptr);

    // return both key ptr and new value ptr
    // old value ptr is returned through old_value
    // old value ptr could be null with a valid version if the key was deleted or the key did not exist
    // the key ptr will be null if the key did not exist
    std::pair<Gptr, TagGptr> destroyC(const char * key, const size_t key_size, TagGptr &old_value);

    // return new value ptr
    // old value ptr is returned through old_value
    // old value ptr could be null with a valid version if the key was deleted
    TagGptr destroyC(Gptr const key_ptr, TagGptr &old_value);

private:
    // when under high contention, current heap implementation may return 0 even if there is free
    // space (false negative)
    // our best option is to retry
    static int const alloc_retry_cnt = 1000;
    struct Node;
    struct TreeStructure;

    Mmgr *mmgr;
    Heap *heap;
    RadixTreeMetrics *metrics;
    Gptr root;

    RadixTree(const RadixTree&);              // disable copying
    RadixTree& operator=(const RadixTree&);   // disable assignment

    //***************************
    // COMMON HELPERS           *
    //***************************
    // convert global address to local pointer
    void* toLocal(const Gptr &gptr);
    void recursive_list(Gptr parent, std::function<void(const char*, const size_t, Gptr)> f, uint64_t &level, uint64_t &depth, uint64_t &value_cnt, uint64_t &node_cnt);
    void recursive_structure(Gptr parent, int level, TreeStructure& structure);
    bool lower_bound(Iter &iter);
    bool next_value(Iter &iter);
};


} // end radixtree

#endif
