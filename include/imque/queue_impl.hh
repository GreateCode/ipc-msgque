#ifndef IMQUE_QUEUE_IMPL_HH
#define IMQUE_QUEUE_IMPL_HH

#include "shared_memory.hh"
#include "allocator.hh"

#include <inttypes.h>
#include <string.h>

namespace imque {
  class QueueImpl {
    struct Entry {
      uint32_t state:1;
      uint32_t value:31;

      enum STATE {
        FREE = 0,
        USED = 1
      };

      uint32_t* uint32_ptr() { return reinterpret_cast<uint32_t*>(this); }
      uint32_t  uint32() const { return *reinterpret_cast<const uint32_t*>(this); }
    };

    struct Stat {
      uint32_t overflowed_count;
    };

    struct Header {
      volatile uint32_t read_pos;
      volatile uint32_t write_pos;
      Stat stat;
      uint32_t entry_count;
      Entry entries[0];
      
      uint32_t next_read_pos() const { return (read_pos+1)%entry_count; }
      uint32_t next_write_pos() const { return (write_pos+1)%entry_count; }
    };

  public:
    QueueImpl(size_t entry_count, SharedMemory& shm)
      : que_(shm.ptr<Header>()),
        alc_(shm.ptr<void>(que_size(entry_count)), shm.size()-que_size(entry_count)) {
      if(shm) {
        que_->entry_count = entry_count;
      }
    }

    operator bool() const { return alc_; }
    
    void init() {
      alc_.init();
      
      que_->read_pos  = 0;
      que_->write_pos = 0;
      que_->stat.overflowed_count = 0;
      memset(que_->entries, 0, sizeof(Entry)*que_->entry_count);
    }

    bool enq(const void* data, size_t size) {
      if(isFull()) {
        __sync_add_and_fetch(&que_->stat.overflowed_count, 1);
        return false;
      }
      
      uint32_t alloc_id = alc_.allocate(sizeof(size_t) + size);
      if(alloc_id == 0) {
        __sync_add_and_fetch(&que_->stat.overflowed_count, 1);
        return false;
      }

      alc_.ptr<size_t>(alloc_id)[0] = size;
      memcpy(alc_.ptr<void>(alloc_id, sizeof(size_t)), data, size);

      if(enq_impl(alloc_id) == false) {
        __sync_add_and_fetch(&que_->stat.overflowed_count, 1);
        alc_.release(alloc_id);
        return false;
      }
      
      return true;
    }

    bool deq(std::string& buf) {
      if(isEmpty()) {
        return false;
      }

      uint32_t alloc_id = deq_impl();
      if(alloc_id == 0) {
        return false;
      }

      size_t size = alc_.ptr<size_t>(alloc_id)[0];
      char*  data = alc_.ptr<char>(alloc_id, sizeof(size_t));
      buf.assign(data, size);
      
      assert(alc_.release(alloc_id));
      
      return true;
    }

    bool isEmpty() const { return que_->read_pos == que_->write_pos; }
    bool isFull()  const { return que_->read_pos == que_->next_write_pos(); }

    static size_t calc_need_byte_size(size_t entry_count, size_t data_size) {
      return que_size(entry_count) + dat_size(data_size);
    }

    size_t overflowedCount() const { return que_->stat.overflowed_count; }
    void resetOverflowedCount() { que_->stat.overflowed_count = 0; }

  private:
    bool enq_impl(uint32_t value) {
      uint32_t curr_read  = que_->read_pos;
      uint32_t curr_write = que_->write_pos;
      uint32_t next_write = (curr_write+1) % que_->entry_count;
      
      if(curr_read == next_write) {
        __sync_add_and_fetch(&que_->stat.overflowed_count, 1);
        return false;
      }
      
      Entry* pe = &que_->entries[curr_write];
      Entry  e = *pe;
      if(e.state != Entry::FREE) {
        __sync_bool_compare_and_swap(&que_->write_pos, curr_write, next_write);
        return enq_impl(value);
      }

      Entry new_e = {Entry::USED, value};
      if(__sync_bool_compare_and_swap(pe->uint32_ptr(), e.uint32(), new_e.uint32()) == false) {
        return enq_impl(value);
      }
      
      __sync_bool_compare_and_swap(&que_->write_pos, curr_write, next_write);      
      return true;
    }

    uint32_t deq_impl() {
      uint32_t curr_read  = que_->read_pos;
      uint32_t curr_write = que_->write_pos;
      uint32_t next_read = (curr_read+1) % que_->entry_count;
      
      if(curr_read == curr_write) {
        return 0;
      }

      Entry* pe = &que_->entries[curr_read];
      Entry   e = *pe;
      if(e.state == Entry::FREE) {
        __sync_bool_compare_and_swap(&que_->read_pos, curr_read, next_read);
        return deq_impl();
      }

      Entry new_e = {Entry::FREE, 0};
      if(__sync_bool_compare_and_swap(pe->uint32_ptr(), e.uint32(), new_e.uint32()) == false) {
        return deq_impl();
      }
      
      __sync_bool_compare_and_swap(&que_->read_pos, curr_read, next_read);
      return e.value;
    }

    static size_t que_size(size_t entry_count) {
      return sizeof(Header) + sizeof(Entry)*entry_count;
    }
    
    static size_t dat_size(size_t data_size) {
      return Allocator::calc_need_byte_size(data_size);
    }

  private:
    Header*   que_;
    Allocator alc_;
  };
}

#endif 