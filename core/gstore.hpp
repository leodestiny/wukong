/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong.html
 *
 */

#pragma once

#include <stdint.h> //uint64_t
#include <vector>
#include <iostream>
#include <pthread.h>
#include <boost/unordered_set.hpp>
#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_unordered_set.h>

#include "config.hpp"
#include "rdma_resource.hpp"
#include "graph_basic_types.hpp"

#include "mymath.hpp"
#include "timer.hpp"
#include "unit.hpp"

class GStore {
private:
    class RDMA_Cache {
        struct Item {
            pthread_spinlock_t lock;
            vertex_t v;
            Item() {
                pthread_spin_init(&lock, 0);
            }
        };

        static const int NUM_ITEMS = 100000;
        Item items[NUM_ITEMS];

    public:
        /// TODO: use more clever cache structure with lock-free implementation
        bool lookup(ikey_t key, vertex_t &ret) {
            if (!global_enable_caching)
                return false;

            int idx = key.hash() % NUM_ITEMS;
            bool found = false;
            pthread_spin_lock(&(items[idx].lock));
            if (items[idx].v.key == key) {
                ret = items[idx].v;
                found = true;
            }
            pthread_spin_unlock(&(items[idx].lock));
            return found;
        }

        void insert(vertex_t &v) {
            if (!global_enable_caching)
                return;

            int idx = v.key.hash() % NUM_ITEMS;
            pthread_spin_lock(&items[idx].lock);
            items[idx].v = v;
            pthread_spin_unlock(&items[idx].lock);
        }
    };

    static const int NUM_LOCKS = 1024;

    static const int MAIN_RATIO = 80; // the percentage of main headers (e.g., 80%)
    static const int ASSOCIATIVITY = 8;  // the associativity of slots in each bucket

    uint64_t sid;
    RdmaResource *rdma;

    vertex_t *vertices;
    edge_t *edges;


    // the size of slot is sizeof(vertex_t)
    // the size of entry is sizeof(edge_t)
    uint64_t num_slots;       // 1 bucket = ASSOCIATIVITY slots
    uint64_t num_buckets;     // main-header region (pre-allocated hash-table)
    uint64_t num_buckets_ext; // indirect-header region (dynamical allocation)
    uint64_t num_entries;     // entry region (dynamical allocation)

    // allocated
    uint64_t last_ext;
    uint64_t last_entry;

    RDMA_Cache rdma_cache;

    pthread_spinlock_t entry_lock;
    pthread_spinlock_t bucket_ext_lock;
    pthread_spinlock_t bucket_locks[NUM_LOCKS]; // lock virtualization (see paper: vLokc CGO'13)

    // cluster chaining hash-table (see paper: DrTM SOSP'15)
    uint64_t insert_key(ikey_t key) {
        uint64_t bucket_id = key.hash() % num_buckets;
        uint64_t slot_id = bucket_id * ASSOCIATIVITY;
        uint64_t lock_id = bucket_id % NUM_LOCKS;

        bool found = false;
        pthread_spin_lock(&bucket_locks[lock_id]);
        while (slot_id < num_slots) {
            // the last slot of each bucket is always reserved for pointer to indirect header
            /// TODO: add type info to slot and resue the last slot to store key
            for (uint64_t i = 0; i < ASSOCIATIVITY - 1; i++, slot_id++) {
                //assert(vertices[slot_id].key != key); // no duplicate key
                if (vertices[slot_id].key == key) {
                    key.print();
                    vertices[slot_id].key.print();
                    assert(false);
                }

                // insert to an empty slot
                if (vertices[slot_id].key == ikey_t()) {
                    vertices[slot_id].key = key;
                    goto done;
                }
            }

            // move to the last slot of bucket and check whether a bucket_ext is used
            if (vertices[++slot_id].key != ikey_t()) {
                slot_id = vertices[slot_id].key.vid * ASSOCIATIVITY;
                continue; // continue and jump to next bucket
            }


            // allocate and link a new indirect header
            pthread_spin_lock(&bucket_ext_lock);
            assert(last_ext < num_buckets_ext);
            vertices[slot_id].key.vid = num_buckets + (last_ext++);
            pthread_spin_unlock(&bucket_ext_lock);

            slot_id = vertices[slot_id].key.vid * ASSOCIATIVITY; // move to a new bucket_ext
            vertices[slot_id].key = key; // insert to the first slot
            goto done;
        }
done:
        pthread_spin_unlock(&bucket_locks[lock_id]);
        assert(slot_id < num_slots);
        assert(vertices[slot_id].key == key);
        return slot_id;
    }

    uint64_t sync_fetch_and_alloc_edges(uint64_t n) {
        uint64_t orig;
        pthread_spin_lock(&entry_lock);
        orig = last_entry;
        last_entry += n;
        assert(last_entry < num_entries);
        pthread_spin_unlock(&entry_lock);
        return orig;
    }

    vertex_t get_vertex_remote(int tid, ikey_t key) {
        int dst_sid = mymath::hash_mod(key.vid, global_num_servers);
        uint64_t bucket_id = key.hash() % num_buckets;
        vertex_t ret;

        if (rdma_cache.lookup(key, ret))
            return ret;

        char *buf = rdma->get_buffer(tid);
        while (true) {
            uint64_t off = bucket_id * ASSOCIATIVITY * sizeof(vertex_t);
            uint64_t sz = ASSOCIATIVITY * sizeof(vertex_t);
            rdma->RdmaRead(tid, dst_sid, buf, sz, off);
            vertex_t *ptr = (vertex_t *)buf;
            for (uint64_t i = 0; i < ASSOCIATIVITY; i++) {
                if (i < ASSOCIATIVITY - 1) {
                    if (ptr[i].key == key) {
                        //we found it
                        rdma_cache.insert(ptr[i]);
                        return ptr[i];
                    }
                } else {
                    if (ptr[i].key != ikey_t()) {
                        //next pointer
                        bucket_id = ptr[i].key.vid;
                        //break from for loop, will go to next bucket
                        break;
                    } else {
                        return vertex_t();
                    }
                }
            }
        }
    }

    vertex_t get_vertex_local(int tid, ikey_t key) {
        uint64_t bucket_id = key.hash() % num_buckets;
        while (true) {
            for (uint64_t i = 0; i < ASSOCIATIVITY; i++) {
                uint64_t slot_id = bucket_id * ASSOCIATIVITY + i;
                if (i < ASSOCIATIVITY - 1) {
                    //data part
                    if (vertices[slot_id].key == key) {
                        //we found it
                        return vertices[slot_id];
                    }
                } else {
                    if (vertices[slot_id].key != ikey_t()) {
                        //next pointer
                        bucket_id = vertices[slot_id].key.vid;
                        //break from for loop, will go to next bucket
                        break;
                    } else {
                        return vertex_t();
                    }
                }
            }
        }
    }

    edge_t *get_edges_remote(int tid, int64_t vid, int64_t d, int64_t pid, int *size) {
        int dst_sid = mymath::hash_mod(vid, global_num_servers);
        ikey_t key = ikey_t(vid, d, pid);
        vertex_t v = get_vertex_remote(tid, key);

        if (v.key == ikey_t()) {
            *size = 0;
            return NULL;
        }

        char *buf = rdma->get_buffer(tid);
        uint64_t off  = num_slots * sizeof(vertex_t) + v.ptr.off * sizeof(edge_t);
        uint64_t sz = v.ptr.size * sizeof(edge_t);
        rdma->RdmaRead(tid, dst_sid, buf, sz, off);
        edge_t *result_ptr = (edge_t *)buf;

        *size = v.ptr.size;
        return result_ptr;
    }

    edge_t *get_edges_local(int tid, int64_t vid, int64_t d, int64_t pid, int *size) {
        ikey_t key = ikey_t(vid, d, pid);
        vertex_t v = get_vertex_local(tid, key);

        if (v.key == ikey_t()) {
            *size = 0;
            return NULL;
        }

        *size = v.ptr.size;
        uint64_t off = v.ptr.off;
        return &(edges[off]);
    }


    typedef tbb::concurrent_hash_map<int64_t, vector< int64_t>> tbb_hash_map;
    typedef tbb::concurrent_unordered_set<int64_t> tbb_unordered_set;

    tbb_hash_map pidx_in_map; // predicate-index (IN)
    tbb_hash_map pidx_out_map; // predicate-index (OUT)
    tbb_hash_map tidx_map; // type-index

    void insert_index_map(tbb_hash_map &map, dir_t d) {
        for (auto const &e : map) {
            int64_t id = e.first;
            uint64_t sz = e.second.size();
            uint64_t off = sync_fetch_and_alloc_edges(sz);

            ikey_t key = ikey_t(0, d, id);
            uint64_t slot_id = insert_key(key);
            iptr_t ptr = iptr_t(sz, off);
            vertices[slot_id].ptr = ptr;

            for (auto const &vid : e.second)
                edges[off++].val = vid;
        }
    }

#ifdef VERSATILE
    tbb_unordered_set p_set; // all of predicates
    tbb_unordered_set v_set; // all of vertices (subjects and objects)

    void insert_index_set(tbb_unordered_set &set, dir_t d) {
        int64_t id = TYPE_ID;
        uint64_t sz = set.size();
        uint64_t off = sync_fetch_and_alloc_edges(sz);

        ikey_t key = ikey_t(0, d, id);
        uint64_t slot_id = insert_key(key);
        iptr_t ptr = iptr_t(sz, off);
        vertices[slot_id].ptr = ptr;

        for (auto const &e : set)
            edges[off++].val = e;
    }
#endif

public:
    // encoding rules of gstore
    // subject/object (vid) >= 2^17, 2^17 > predicate/type (p/tid) > 2^1,
    // TYPE_ID = 1, PREDICATE_ID = 0, OUT = 1, IN = 0
    //
    // NORMAL key/value pair
    //   key = [vid |    predicate | IN/OUT]  value = [vid0, vid1, ..]  i.e., vid's ngbrs w/ predicate
    //   key = [vid |      TYPE_ID |    OUT]  value = [tid0, tid1, ..]  i.e., vid's all types
    //   key = [vid | PREDICATE_ID | IN/OUT]  value = [pid0, pid1, ..]  i.e., vid's all predicates
    // INDEX key/value pair
    //   key = [  0 |          pid | IN/OUT]  value = [vid0, vid1, ..]  i.e., predicate-index
    //   key = [  0 |          tid |     IN]  value = [vid0, vid1, ..]  i.e., type-index
    //   key = [  0 |      TYPE_ID |    OUT]  value = [vid0, vid1, ..]  i.e., all objects/subjects
    //   key = [  0 |      TYPE_ID |    OUT]  value = [vid0, vid1, ..]  i.e., all predicates
    // Empty key
    //   key = [  0 |            0 |      0]  value = [vid0, vid1, ..]  i.e., init

    // GStore: key (main-header and indirect-header region) | value (entry region)
    // The key (head region) is a cluster chaining hash-table (with associativity)
    // The value (entry region) is a varying-size array
    GStore(RdmaResource *rdma, uint64_t sid): rdma(rdma), sid(sid) {
        num_slots = global_num_keys_million * 1000 * 1000;
        num_buckets = (uint64_t)((num_slots / ASSOCIATIVITY) * MAIN_RATIO / 100);
        //num_buckets_ext = (num_slots / ASSOCIATIVITY) / (KEY_RATIO + 1);
        num_buckets_ext = (num_slots / ASSOCIATIVITY) - num_buckets;

        vertices = (vertex_t *)(rdma->get_kvs());
        edges = (edge_t *)(rdma->get_kvs() + num_slots * sizeof(vertex_t));

        if (rdma->get_kvs_size() <= num_slots * sizeof(vertex_t)) {
            cout << "ERROR: " << global_memstore_size_gb
                 << "GB memory store is not enough to store hash table with "
                 << global_num_keys_million << "M keys" << std::endl;
            assert(false);
        }

        num_entries = (rdma->get_kvs_size() - num_slots * sizeof(vertex_t)) / sizeof(edge_t);
        last_entry = 0;

        pthread_spin_init(&entry_lock, 0);
        pthread_spin_init(&bucket_ext_lock, 0);
        for (int i = 0; i < NUM_LOCKS; i++)
            pthread_spin_init(&bucket_locks[i], 0);
    }

    void init() {
        // initiate keys
        #pragma omp parallel for num_threads(global_num_engines)
        for (uint64_t i = 0; i < num_slots; i++)
            vertices[i].key = ikey_t();
    }

    // skip all TYPE triples (e.g., <http://www.Department0.University0.edu> rdf:type ub:University)
    // because Wukong treats all TYPE triples as index vertices. In addition, the triples in triple_ops
    // has been sorted by the vid of object, and IDs of types are always smaller than normal vertex IDs.
    // Consequently, all TYPE triples are aggregated at the beggining of triple_ops
    void insert_normal(vector<triple_t> &spo, vector<triple_t> &ops) {
        // treat type triples as index vertices
        uint64_t type_triples = 0;
        while (type_triples < ops.size() && is_tpid(ops[type_triples].o))
            type_triples++;

#ifdef VERSATILE
        // the number of separate combinations of subject/object and predicate
        uint64_t accum_predicate = 0;
#endif
        // allocate edges in entry region for triples
        uint64_t off = sync_fetch_and_alloc_edges(spo.size() + ops.size() - type_triples);

        uint64_t s = 0;
        while (s < spo.size()) {
            // predicate-based key (subject + predicate)
            uint64_t e = s + 1;
            while ((e < spo.size())
                    && (spo[s].s == spo[e].s)
                    && (spo[s].p == spo[e].p))  { e++; }
#ifdef VERSATILE
            accum_predicate++;
#endif
            // insert vertex
            ikey_t key = ikey_t(spo[s].s, OUT, spo[s].p);
            uint64_t slot_id = insert_key(key);
            iptr_t ptr = iptr_t(e - s, off);
            vertices[slot_id].ptr = ptr;

            // insert edges
            for (uint64_t i = s; i < e; i++)
                edges[off++].val = spo[i].o;

            s = e;
        }

        s = type_triples;
        while (s < ops.size()) {
            // predicate-based key (object + predicate)
            uint64_t e = s + 1;
            while ((e < ops.size())
                    && (ops[s].o == ops[e].o)
                    && (ops[s].p == ops[e].p)) { e++; }
#ifdef VERSATILE
            accum_predicate++;
#endif
            // insert vertex
            ikey_t key = ikey_t(ops[s].o, IN, ops[s].p);
            uint64_t slot_id = insert_key(key);
            iptr_t ptr = iptr_t(e - s, off);
            vertices[slot_id].ptr = ptr;

            // insert edges
            for (uint64_t i = s; i < e; i++)
                edges[off++].val = ops[i].s;

            s = e;
        }

#ifdef VERSATILE
        // The following code is used to support a rare case where the predicate is unknown
        // (e.g., <http://www.Department0.University0.edu> ?P ?O). Each normal vertex should
        // add two key/value pairs with a reserved ID (i.e., PREDICATE_ID) as the predicate
        // to store the IN and OUT lists of its predicates.
        // e.g., key=(vid, PREDICATE_ID, IN/OUT), val=(predicate0, predicate1, ...)
        //
        // NOTE, it is disabled by default in order to save memory.

        // allocate edges in entry region for special PREDICATE triples
        off = sync_fetch_and_alloc_edges(accum_predicate);

        s = 0;
        while (s < spo.size()) {
            // insert vertex
            ikey_t key = ikey_t(spo[s].s, OUT, PREDICATE_ID);
            uint64_t slot_id = insert_key(key);

            // insert edges
            uint64_t e = s, sz = 0;
            do {
                uint64_t m = e;
                edges[off++].val = spo[e++].p; // insert a new predicate
                sz++;

                // skip the triples with the same subject and predicate
                while ((e < spo.size())
                        && (spo[s].s == spo[e].s)
                        && (spo[m].p == spo[e].p)) { e++; }
            } while (e < spo.size() && spo[s].s == spo[e].s);

            // link to edges
            iptr_t ptr = iptr_t(sz, off - sz);
            vertices[slot_id].ptr = ptr;

            s = e;
        }

        s = type_triples;
        while (s < ops.size()) {
            // insert vertex
            ikey_t key = ikey_t(ops[s].o, IN, PREDICATE_ID);
            uint64_t slot_id = insert_key(key);

            // insert edges
            uint64_t e = s, sz = 0;
            do {
                uint64_t m = e;
                edges[off++].val = ops[e++].p; // insert a new predicate
                sz++;

                // skip the triples with the same object and predicate
                while ((e < ops.size())
                        && (ops[s].o == ops[e].o)
                        && (ops[m].p == ops[e].p)) { e++; }
            } while (e < ops.size() && ops[s].o == ops[e].o);

            // link to edges
            iptr_t ptr = iptr_t(sz, off - sz);
            vertices[slot_id].ptr = ptr;

            s = e;
        }
#endif
    }

    void insert_index() {
        uint64_t t1 = timer::get_usec();

        // scan raw data to generate index data in parallel
        #pragma omp parallel for num_threads(global_num_engines)
        for (int bucket_id = 0; bucket_id < num_buckets + num_buckets_ext; bucket_id++) {
            uint64_t slot_id = bucket_id * ASSOCIATIVITY;
            for (int i = 0; i < ASSOCIATIVITY - 1; i++, slot_id++) {
                // empty slot, skip it
                if (vertices[slot_id].key == ikey_t()) continue;

                int64_t vid = vertices[slot_id].key.vid;
                int64_t pid = vertices[slot_id].key.pid;
                dir_t dir = (dir_t)vertices[slot_id].key.dir;

                uint64_t sz = vertices[slot_id].ptr.size;
                uint64_t off = vertices[slot_id].ptr.off;

                if (dir == IN) {
                    if (pid == PREDICATE_ID) {
#ifdef VERSATILE
                        v_set.insert(vid);
                        for (uint64_t e = 0; e < sz; e++)
                            p_set.insert(edges[off + e].val);
#endif
                    } else if (pid == TYPE_ID) {
                        assert(false); // (IN) type triples should be skipped
                    } else { // predicate-index (OUT) vid
                        tbb_hash_map::accessor a;
                        pidx_out_map.insert(a, pid);
                        a->second.push_back(vid);
                    }
                } else {
                    if (pid == PREDICATE_ID) {
#ifdef VERSATILE
                        v_set.insert(vid);
                        for (uint64_t e = 0; e < sz; e++)
                            p_set.insert(edges[off + e].val);
#endif
                    } else if (pid == TYPE_ID) {
                        // type-index (IN) vid
                        for (uint64_t e = 0; e < sz; e++) {
                            tbb_hash_map::accessor a;
                            tidx_map.insert(a, edges[off + e].val);
                            a->second.push_back(vid);
                        }
                    } else { // predicate-index (IN) vid
                        tbb_hash_map::accessor a;
                        pidx_in_map.insert(a, pid);
                        a->second.push_back(vid);
                    }
                }
            }
        }

        uint64_t t2 = timer::get_usec();
        cout << (t2 - t1) / 1000 << " ms for (parallel) prepare index info" << endl;

        // add type/predicate index vertices
        insert_index_map(tidx_map, IN);
        insert_index_map(pidx_in_map, IN);
        insert_index_map(pidx_out_map, OUT);

        tbb_hash_map().swap(pidx_in_map);
        tbb_hash_map().swap(pidx_out_map);
        tbb_hash_map().swap(tidx_map);

#ifdef VERSATILE
        insert_index_set(v_set, IN);
        insert_index_set(p_set, OUT);

        tbb_unordered_set().swap(v_set);
        tbb_unordered_set().swap(p_set);
#endif

        uint64_t t3 = timer::get_usec();
        cout << (t3 - t2) / 1000 << " ms for insert index data into gstore" << endl;
    }

    edge_t *get_edges_global(int tid, int64_t vid, int64_t d, int64_t pid, int *sz) {
        if (mymath::hash_mod(vid, global_num_servers) == sid)
            return get_edges_local(tid, vid, d, pid, sz);
        else
            return get_edges_remote(tid, vid, d, pid, sz);
    }

    edge_t *get_index_edges_local(int tid, int64_t pid, int64_t d, int *sz) {
        // predicate is not important, so we set it 0
        return get_edges_local(tid, 0, d, pid, sz);
    }

    // analysis and debuging
    void print_mem_usage() {
        uint64_t used_slots = 0;
        for (int x = 0; x < num_buckets; x++) {
            uint64_t slot_id = x * ASSOCIATIVITY;
            for (int y = 0; y < ASSOCIATIVITY - 1; y++, slot_id++) {
                if (vertices[slot_id].key == ikey_t())
                    continue;
                used_slots++;
            }
        }

        cout << "main header: " << B2MiB(num_buckets * ASSOCIATIVITY * sizeof(vertex_t))
             << " MB (" << num_buckets * ASSOCIATIVITY << " slots)" << endl;
        cout << "\tused: " << 100.0 * used_slots / (num_buckets * ASSOCIATIVITY)
             << " % (" << used_slots << " slots)" << endl;
        cout << "\tchain: " << 100.0 * num_buckets / (num_buckets * ASSOCIATIVITY)
             << " % (" << num_buckets << " slots)" << endl;

        used_slots = 0;
        for (int x = num_buckets; x < num_buckets + num_buckets_ext; x++) {
            uint64_t slot_id = x * ASSOCIATIVITY;
            for (int y = 0; y < ASSOCIATIVITY - 1; y++, slot_id++) {
                if (vertices[slot_id].key == ikey_t())
                    continue;
                used_slots++;
            }
        }

        cout << "indirect header: " << B2MiB(num_buckets_ext * ASSOCIATIVITY * sizeof(vertex_t))
             << " MB (" << num_buckets_ext * ASSOCIATIVITY << " slots)" << endl;
        cout << "\talloced: " << 100.0 * last_ext / num_buckets_ext
             << " % (" << last_ext << " buckets)" << endl;
        cout << "\tused: " << 100.0 * used_slots / (num_buckets_ext * ASSOCIATIVITY)
             << " % (" << used_slots << " slots)" << endl;

        cout << "entry: " << B2MiB(num_entries * sizeof(edge_t))
             << " MB (" << num_entries << " entries)" << endl;
        cout << "\tused: " << 100.0 * last_entry / num_entries
             << " % (" << last_entry << " entries)" << endl;


        int sz = 0;
        get_edges_local(0, 0, IN, TYPE_ID, &sz);
        cout << "#vertices: " << sz << endl;
        get_edges_local(0, 0, OUT, TYPE_ID, &sz);
        cout << "#predicates: " << sz << endl;
    }
};