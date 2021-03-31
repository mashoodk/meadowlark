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


#include <stddef.h>
#include <stdint.h>
#include <iostream>
#include <string>
#include <string.h> // memset, memcpy
#include <utility> // pair
#include <bitset>

#include "nvmm/error_code.h"
#include "nvmm/global_ptr.h"
#include "nvmm/shelf_id.h"
#include "nvmm/memory_manager.h"
#include "nvmm/heap.h"

#include "nvmm/fam.h"
#include "kvs_radix_tree_tiny.h"

#define CHAR2UINT64(x) (*(uint64_t *)x)
#define CHAR2UINT64_CONST(x) (*(uint64_t const *)x)

namespace radixtree {

KVSRadixTreeTiny::KVSRadixTreeTiny(Gptr root, std::string base, std::string user, size_t heap_size, nvmm::PoolId heap_id, KVSMetrics* metrics)
    : heap_id_(heap_id), heap_size_(heap_size),
      mmgr_(Mmgr::GetInstance()), emgr_(Emgr::GetInstance()), heap_(nullptr),
      tree_(nullptr), root_(root), metrics_(metrics) {
    int ret = Open();
    assert(ret==0);
}

KVSRadixTreeTiny::~KVSRadixTreeTiny() {
    int ret = Close();
    assert(ret==0);
}

void KVSRadixTreeTiny::Maintenance() {
    heap_->OfflineFree();
}

int KVSRadixTreeTiny::Open() {
    nvmm::ErrorCode ret;

    // find the heap
    heap_ = mmgr_->FindHeap(heap_id_);
    if(!heap_) {
        ret = mmgr_->CreateHeap(heap_id_, heap_size_);
        if(ret!=nvmm::NO_ERROR)
            return -1;
        heap_ = mmgr_->FindHeap(heap_id_);
    }
    assert(heap_);

    // open the heap
    ret = heap_->Open();
    if(ret!=nvmm::NO_ERROR) {
        delete heap_;
        return -1;
    }

    // create/open the radixtree
// #ifdef DEBUG
//     if(!root_)
//         std::cout << "create a new radix tree: ";
//     else
//         std::cout << "open an existing radix tree: ";
// #endif
    tree_ = new RadixTree(mmgr_, heap_, static_cast<RadixTreeMetrics*>(metrics_), root_);
    if (!tree_) {
        delete heap_;
        return -1;
    }
    root_ = tree_->get_root();
// #ifdef DEBUG
//     std::cout << (uint64_t)root_ << std::endl;
// #endif
    return 0;
}

int KVSRadixTreeTiny::Close() {
    nvmm::ErrorCode ret;

    // close the radix tree
    if (tree_) {
// #ifdef DEBUG
//         std::cout << "close the radix tree: " << root_;
// #endif
        delete tree_;
        tree_ = nullptr;
    }

    // close the heap
    if (heap_ && heap_->IsOpen()) {
        ret = heap_->Close();
        if(ret!=nvmm::NO_ERROR)
            return -1;
        delete heap_;
        heap_ = nullptr;
    }

    // delete all iters
    for (auto iter:iters_) {
        if (iter)
            delete iter;
    }
    return 0;
}

int KVSRadixTreeTiny::Put (char const *key, size_t const key_len,
                           char const *val, size_t const val_len) {
    //std::cout << "PUT" << " " << std::string(key, key_len) << " " << std::string(val, val_len) << std::endl;

    if (key_len > kMaxKeyLen)
        return -1;
    if (val_len > kMaxValLen)
        return -1;

    Gptr val_gptr = CHAR2UINT64_CONST(val);
    TagGptr old_value = tree_->put(key, key_len, val_gptr, UPDATE);
    return 0;
}

int KVSRadixTreeTiny::Get (char const *key, size_t const key_len,
                           char *val, size_t &val_len) {
    //std::cout << "GET" << " " << std::string(key, key_len) << std::endl;

    if (key_len > kMaxKeyLen)
        return -1;
    // if (val_len > kMaxValLen)
    //     return -1;

    TagGptr val_ptr = tree_->get(key, key_len);
    CHAR2UINT64(val) = val_ptr.gptr_nomark();
    val_len=kMaxValLen;
    if (!val_ptr.IsValid()) {
        //std::cout << val_ptr.gptr_nomark() << std::endl;
        return -2;
    }
    return 0;
}

int KVSRadixTreeTiny::Del (char const *key, size_t const key_len) {
    //std::cout << "DEL" << " " << std::string(key, key_len) << std::endl;
    if (key_len > kMaxKeyLen)
        return -1;

    TagGptr val_ptr = tree_->destroy(key, key_len);
    if (!val_ptr.IsValid()) {
        //std::cout << val_ptr.gptr_nomark() << std::endl;
        return -2;
    }
    return 0;
}

int KVSRadixTreeTiny::Scan (
    int &iter_handle,
    char *key, size_t &key_len,
    char *val, size_t &val_len,
    char const *begin_key, size_t const begin_key_len,
    bool const begin_key_inclusive,
    char const *end_key, size_t const end_key_len,
    bool const end_key_inclusive) {

    if (begin_key_len > kMaxKeyLen || end_key_len > kMaxKeyLen)
        return -1;
    if (key_len > kMaxKeyLen)
        return -1;
    if (val_len > kMaxValLen)
        return -1;

    RadixTree::Iter *iter=new RadixTree::Iter();
    TagGptr val_gptr;
    int ret = tree_->scan(*iter,
                          key, key_len, val_gptr,
                          begin_key, begin_key_len, begin_key_inclusive,
                          end_key, end_key_len, end_key_inclusive);
    if (ret!=0)
        return -2; // no key in range

    CHAR2UINT64(val) = val_gptr.gptr_nomark();
    val_len=kMaxValLen;

    // assign iter handle
    std::lock_guard<std::mutex> lock(mutex_);
    iters_.push_back(iter);
    iter_handle = (int)(iters_.size()-1);
    return 0;
}

int KVSRadixTreeTiny::GetNext(int iter_handle,
                          char *key, size_t &key_len,
                          char *val, size_t &val_len) {
    if (key_len > kMaxKeyLen)
        return -1;
    if (val_len > kMaxValLen)
        return -1;
    if (iter_handle <0 || iter_handle >= (int)iters_.size())
        return -1;

    TagGptr val_gptr;

    RadixTree::Iter *iter;
    {
        //std::lock_guard<std::mutex> lock(mutex_);
        iter = iters_[iter_handle];
    }
    int ret = tree_->get_next(*iter,
                              key, key_len, val_gptr);
    if (ret!=0)
        return -2; // no next key

    CHAR2UINT64(val) = val_gptr.gptr_nomark();
    val_len=kMaxValLen;

    return 0;
}


/*
  for consistent DRAM caching
*/
int KVSRadixTreeTiny::Put (char const *key, size_t const key_len,
                       char const *val, size_t const val_len,
                       Gptr &key_ptr, TagGptr &val_ptr) {
    //std::cout << "PUT" << " " << std::string(key, key_len) << " " << std::string(val, val_len) << std::endl;

    if (key_len > kMaxKeyLen)
        return -1;
    if (val_len > kMaxValLen)
        return -1;

    Gptr val_gptr = CHAR2UINT64_CONST(val);
    TagGptr old_value;
    std::pair<Gptr, TagGptr> kv_ptr = tree_->putC(key, key_len, val_gptr, old_value);
    key_ptr = kv_ptr.first;
    val_ptr = kv_ptr.second;

    return 0;
}

int KVSRadixTreeTiny::Put (Gptr const key_ptr, TagGptr &val_ptr,
                       char const *val, size_t const val_len) {
    //std::cout << "PUT" << " " << std::string(key, key_len) << " " << std::string(val, val_len) << std::endl;

    if (val_len > kMaxValLen)
        return -1;

    Gptr val_gptr = CHAR2UINT64_CONST(val);
    TagGptr old_value;
    val_ptr = tree_->putC(key_ptr, val_gptr, old_value);
    return 0;
}

int KVSRadixTreeTiny::Get (char const *key, size_t const key_len,
                       char *val, size_t &val_len,
                       Gptr &key_ptr, TagGptr &val_ptr) {
    //std::cout << "GET" << " " << std::string(key, key_len) << std::endl;

    if (key_len > kMaxKeyLen)
        return -1;

    std::pair<Gptr, TagGptr> kv_ptr = tree_->getC(key, key_len);

    key_ptr=kv_ptr.first; // key_ptr could be null
    val_ptr=kv_ptr.second; // val_ptr could be null

    if(!kv_ptr.first.IsValid()) {
        // key node does not exist
        return 0;
    }
    // key node exists
    if (kv_ptr.second.IsValid()) {
        CHAR2UINT64(val) = val_ptr.gptr_nomark();
        val_len=kMaxValLen;
    }
    return 0;
}

int KVSRadixTreeTiny::Get (Gptr const key_ptr, TagGptr &val_ptr,
                       char *val, size_t &val_len, bool get_value) {
    //std::cout << "GET" << " " << std::string(key, key_len) << std::endl;

    TagGptr val_ptr_cur = tree_->getC(key_ptr);

    if(val_ptr_cur == val_ptr && get_value==false) {
        // val_ptr is not stale
        return 0;
    }
    else {
        // val_ptr is stale or we always want to get the value
        val_ptr=val_ptr_cur;
        CHAR2UINT64(val) = val_ptr.gptr_nomark();
        val_len=kMaxValLen;
        return 0;
    }
}


int KVSRadixTreeTiny::Del (char const *key, size_t const key_len,
                       Gptr &key_ptr, TagGptr &val_ptr){
    //std::cout << "DEL" << " " << std::string(key, key_len) << std::endl;
    if (key_len > kMaxKeyLen)
        return -1;

    TagGptr old_value;
    std::pair<Gptr, TagGptr> kv_ptr = tree_->destroyC(key, key_len, old_value);

    key_ptr=kv_ptr.first; // key_ptr could be null
    val_ptr=kv_ptr.second; // val_ptr must be null

    return 0;
}


int KVSRadixTreeTiny::Del (Gptr const key_ptr, TagGptr &val_ptr) {
    //std::cout << "DEL" << " " << std::string(key, key_len) << std::endl;
    TagGptr old_value;
    val_ptr = tree_->destroyC(key_ptr, old_value);
    return 0;
}

void KVSRadixTreeTiny::ReportMetrics() {
    if (metrics_) {
        metrics_->Report();
    }
}

int KVSRadixTreeTiny::FindOrCreate (char const *key, size_t const key_len,
                           char const *val, size_t const val_len,
                char *ret_val, size_t &ret_len)
{
    return 0;
}


} // namespace radixtree
