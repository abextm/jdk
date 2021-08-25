/*
 * Copyright (c) 2021, Huawei and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_G1_G1EVACUATIONFAILUREOBJSINHR_HPP
#define SHARE_GC_G1_G1EVACUATIONFAILUREOBJSINHR_HPP

#include "memory/iterator.hpp"
#include "oops/oop.hpp"

// This class
//   1. records the objects per region which have failed to evacuate.
//   2. speeds up removing self forwarded ptrs in post evacuation phase.
//
class G1EvacuationFailureObjsInHR {
  template<uint32_t LEN, typename Elem>
  class Array;

  template<uint32_t LEN, typename Elem>
  class Node : public CHeapObj<mtGC>{
    friend G1EvacuationFailureObjsInHR;
    friend Array<LEN, Elem>;

  private:
    static const uint32_t LENGTH = LEN;
    static const size_t SIZE = LENGTH * sizeof(Elem);
    Elem* _oop_offsets;

  public:
    Node() {
      _oop_offsets = (Elem*)AllocateHeap(SIZE, mtGC);
    }
    Elem& operator[] (size_t idx) {
      return _oop_offsets[idx];
    }
    static Node<LEN, Elem>* create_node() {
      return new Node<LEN, Elem>();
    }
    static void free_node(Node<LEN, Elem>* node) {
      FreeHeap(node->_oop_offsets);
      delete(node);
    }
  };

  template<uint32_t NODE_SIZE, typename Elem>
  class Array : public CHeapObj<mtGC> {
  public:
    typedef Node<NODE_SIZE, Elem> NODE_XXX;

  private:
    static const uint64_t TMP = 1l << 32;
    static const uint64_t LOW_MASK = TMP - 1;
    static const uint64_t HIGH_MASK = LOW_MASK << 32;
    const uint32_t _max_nodes_length;

    volatile uint64_t _cur_pos;
    NODE_XXX* volatile * _nodes;
    volatile uint _elements_num;

  private:
    uint64_t low(uint64_t n) {
      return (n & LOW_MASK);
    }
    uint64_t high(uint64_t n) {
      return (n & HIGH_MASK);
    }
    uint32_t elem_index(uint64_t n) {
      assert(low(n) < NODE_XXX::LENGTH, "must be");
      return low(n);
    }
    uint32_t node_index(uint64_t n) {
      return high(n) >> 32;
    }

    uint64_t next(uint64_t n) {
      uint64_t lo = low(n);
      uint64_t hi = high(n);
      assert((lo < NODE_XXX::LENGTH) && (NODE_XXX::LENGTH <= LOW_MASK), "must be");
      assert(hi < HIGH_MASK, "must be");
      if ((lo+1) == NODE_XXX::LENGTH) {
        lo = 0;
        hi += (1l << 32);
      } else {
        lo++;
      }
      assert(hi <= HIGH_MASK, "must be");
      return hi | lo;
    }

  public:
    Array(uint32_t max_nodes_length) : _max_nodes_length(max_nodes_length) {
      _nodes = (NODE_XXX**)AllocateHeap(_max_nodes_length * sizeof(NODE_XXX*), mtGC);
      for (uint32_t i = 0; i < _max_nodes_length; i++) {
        Atomic::store(&_nodes[i], (NODE_XXX *)NULL);
      }

      Atomic::store(&_elements_num, 0u);
      Atomic::store(&_cur_pos, (uint64_t)0);
    }

    ~Array() {
      reset();
      FreeHeap((NODE_XXX**)_nodes);
    }

    uint objs_num() {
      return Atomic::load(&_elements_num);
    }

    void add(Elem elem) {
      while (true) {
        uint64_t pos = Atomic::load(&_cur_pos);
        uint64_t next_pos = next(pos);
        uint64_t res = Atomic::cmpxchg(&_cur_pos, pos, next_pos);
        if (res == pos) {
          uint32_t hi = node_index(pos);
          uint32_t lo = elem_index(pos);
          if (lo == 0) {
            Atomic::store(&_nodes[hi], NODE_XXX::create_node());
          }
          NODE_XXX* node = NULL;
          while ((node = Atomic::load(&_nodes[hi])) == NULL);

          node->operator[](lo) = elem;
          Atomic::inc(&_elements_num);
          break;
        }
      }
    }

    template<typename VISITOR>
    void iterate_elements(VISITOR v) {
      int64_t pos = Atomic::load(&_cur_pos);
      DEBUG_ONLY(uint total = 0);
      uint32_t hi = node_index(pos);
      uint32_t lo = elem_index(pos);
      for (uint32_t i = 0; i <= hi; i++) {
        uint32_t limit = (i == hi) ? lo : NODE_XXX::LENGTH;
        NODE_XXX* node = Atomic::load(&_nodes[i]);
        for (uint32_t j = 0; j < limit; j++) {
          v->visit(node->operator[](j));
          DEBUG_ONLY(total++);
        }
      }
      assert(total == Atomic::load(&_elements_num), "must be");
    }

    template<typename VISITOR>
    void iterate_nodes(VISITOR v) {
      int64_t pos = Atomic::load(&_cur_pos);
      uint32_t hi = node_index(pos);
      uint32_t lo = elem_index(pos);
      for (uint32_t i = 0; i <= hi; i++) {
        NODE_XXX* node = Atomic::load(&_nodes[i]);
        uint32_t limit = (i == hi) ? lo : NODE_XXX::LENGTH;
        v->visit(node, limit);
      }
    }

    void reset() {
      int64_t pos = Atomic::load(&_cur_pos);
      for (uint32_t hi = 0; hi <= node_index(pos); hi++) {
        NODE_XXX::free_node(_nodes[hi]);
        Atomic::store(&_nodes[hi], (NODE_XXX *)NULL);
      }
      Atomic::store(&_elements_num, 0u);
      Atomic::store(&_cur_pos, (uint64_t)0);
    }
  };

public:
  typedef uint32_t Elem;

private:
  static const uint32_t NODE_LENGTH = 256;
  const uint64_t offset_mask;
  const uint _region_idx;
  const HeapWord* _bottom;
  Array<NODE_LENGTH, Elem> _nodes_array;
  Elem* _offset_array;
  uint _objs_num;

private:
  oop cast_from_offset(Elem offset) {
    return oop(_bottom + offset);
  }
  Elem cast_from_oop_addr(oop obj) {
    const HeapWord* o = cast_from_oop<const HeapWord*>(obj);
    size_t offset = pointer_delta(o, _bottom);
    assert(offset_mask >= offset, "must be");
    return offset & offset_mask;
  }
  void visit(Elem);
  void visit(Array<NODE_LENGTH, Elem>::NODE_XXX* node, uint32_t limit);
  void compact();
  void sort();
  void clear_array();
  void iterate_internal(ObjectClosure* closure);

public:
  G1EvacuationFailureObjsInHR(uint region_idx, HeapWord* bottom);
  ~G1EvacuationFailureObjsInHR();

  void record(oop obj);
  void iterate(ObjectClosure* closure);
};


#endif //SHARE_GC_G1_G1EVACUATIONFAILUREOBJSINHR_HPP
