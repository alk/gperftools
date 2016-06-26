// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#define _GNU_SOURCE 1

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <time.h>
#include <sys/mman.h>
#include <signal.h>

#include "varint_codec.h"
#include "malloc_trace_encoder.h"

#define PREDICT_FALSE(cond) __builtin_expect((cond), 0)
#define PREDICT_TRUE(cond) __builtin_expect((cond), 1)

struct EventUnion {
  uint8_t type;
  bool new_thread;
  bool buf_end;
  uint32_t cpu;
  uint64_t ts;
  union {
    events::Malloc malloc;
    events::Free free;
    events::Realloc realloc;
    events::Memalign memalign;
    events::Tok tok;
    events::Death death;
    events::Buf buf;
  };

  void print(void) const {
    switch (type) {
    case EventsEncoder::kEventMalloc:
      printf("<malloc size=% 8d ts=%016llu cpu=%d token=%llu thread_id=%d>\n",
             (int)(malloc.size), (unsigned long long)ts, (int)cpu,
             (unsigned long long)(malloc.token), (int)(malloc.thread_id));
      break;
    case EventsEncoder::kEventFree:
      printf("<free ts=%llu cpu=%d token=%llu thread_id=%d>\n",
             (unsigned long long)ts, (int)cpu,
             (unsigned long long)(free.token), (int)(free.thread_id));
      break;
    case EventsEncoder::kEventTok:
    case EventsEncoder::kEventBuf:
    case EventsEncoder::kEventDeath:
    case EventsEncoder::kEventEnd:
      printf("<misc>\n");
      break;
    case EventsEncoder::kEventRealloc:
      printf("<realloc size=%d ts=%llu cpu=%d old_token=%llu new_token=%llu thread_id=%d>\n",
             (int)(realloc.new_size), (unsigned long long)ts, (int)cpu,
             (unsigned long long)(realloc.old_token), (unsigned long long)(realloc.new_token),
             (int)(realloc.thread_id));
      break;
    case EventsEncoder::kEventMemalign:
      printf("<memalign size=%d ts=%llu cpu=%d token=%llu thread_id=%d alignment=%d>\n",
             (int)(memalign.size), (unsigned long long)ts, (int)cpu,
             (unsigned long long)(memalign.token), (int)(memalign.thread_id),
             (int)(memalign.alignment));
      break;
    }
  }
};

class ThreadState {
public:
  explicit ThreadState(uint64_t thread_id) : thread_id(thread_id) {}

  uint64_t thread_id;
  uint64_t prev_size = 0;
  uint64_t prev_token = 0;
  uint64_t malloc_tok_seq = 0;
  uint32_t last_cpu;
  uint64_t last_ts;

  void consume_malloc(EventUnion *u, uint64_t first_word) {
    u->type = EventsEncoder::kEventMalloc;
    u->cpu = last_cpu;
    u->ts = last_ts;
    u->malloc.thread_id = thread_id;
    EventsEncoder::decode_malloc(&u->malloc, first_word, &prev_size, &malloc_tok_seq);
  }

  void consume_free(EventUnion *u, uint64_t first_word) {
    u->type = EventsEncoder::kEventFree;
    u->cpu = last_cpu;
    u->ts = last_ts;
    u->free.thread_id = thread_id;
    EventsEncoder::decode_free(&u->free, first_word, &prev_token);
  }

  void consume_realloc(EventUnion *u, uint64_t first_word, uint64_t second_word) {
    u->type = EventsEncoder::kEventRealloc;
    u->cpu = last_cpu;
    u->ts = last_ts;
    u->realloc.thread_id = thread_id;
    EventsEncoder::decode_realloc(&u->realloc, first_word, second_word,
                                  &prev_size, &prev_token, &malloc_tok_seq);
  }

  void consume_memalign(EventUnion *u, uint64_t first_word, uint64_t second_word) {
    u->type = EventsEncoder::kEventMemalign;
    u->cpu = last_cpu;
    u->ts = last_ts;
    u->memalign.thread_id = thread_id;
    EventsEncoder::decode_memalign(&u->memalign, first_word, second_word,
                                   &prev_size, &malloc_tok_seq);
  }

  void consume_tok(events::Tok &t) {
    malloc_tok_seq = t.token_base;
  }
};

class EventsStream {
public:
  typedef std::function<int (void *, size_t)> reader_fn_t;
  explicit EventsStream(reader_fn_t reader_fn) : reader_fn(reader_fn) {}

  reader_fn_t reader_fn;
  std::unordered_map<uint64_t, ThreadState> thread_states;
  char *buf_ptr = buf;
  char *buf_end = buf;

  ThreadState *curr_thread = nullptr;
  int64_t buf_size_left = 0;
  bool have_buf = false;
  bool end_of_buf = false;

  events::Buf last_buf;

  bool done = false;

  // TODO: configurable page size
  char buf[1 << 20] __attribute__((aligned(4096)));

  bool has_next() {return !done;}

  void fill_buf() {
    assert(buf_ptr + 10 > buf_end);
    char *cp_start = buf + (reinterpret_cast<uint64_t>(buf_ptr) & 4095);
    memmove(cp_start, buf_ptr, buf_end - buf_ptr);
    buf_end -= (buf_ptr - cp_start);
    buf_ptr = cp_start;

    int rv = reader_fn(buf_end, buf + sizeof(buf) - buf_end);
    if (rv < 0) {
      perror("read");
      assert(false);
      abort();
    }
    buf_end += rv;
    assert(buf_end != buf);
  }

  uint64_t read_varint() {
    if (PREDICT_FALSE(buf_ptr + 10 > buf_end)) {
      fill_buf();
    }

    VarintCodec::DecodeResult<uint64_t> res = VarintCodec::decode_unsigned(buf_ptr);

    buf_ptr += res.advance;
    assert(buf_ptr <= buf_end);

    buf_size_left -= res.advance;
    if (PREDICT_FALSE(buf_size_left <= 0)) {
      if (PREDICT_FALSE(!have_buf)) {
        buf_size_left = ((uint64_t)-1LL) >> 1;
      } else {
        assert(buf_size_left >= 0);
        end_of_buf = true;
        have_buf = false;
      }
    }
    return res.value;
  }

  EventUnion next() {
    EventUnion rv;
    rv.new_thread = false;
    rv.buf_end = true;

    if (PREDICT_FALSE(end_of_buf)) {
      end_of_buf = false;
      rv.buf_end = true;
      rv.type = EventsEncoder::kEventBuf;
      rv.buf = last_buf;

      curr_thread->last_cpu = rv.buf.cpu;
      curr_thread->last_ts = rv.buf.ts;

      return rv;
    }

    uint64_t first_word = read_varint();
    // somewhat optimized handling of malloc & free. Our most common
    // types.
    if (!(first_word & 0x2)) {
      if ((first_word & 0x1)) {
        curr_thread->consume_free(&rv, first_word);
      } else {
        curr_thread->consume_malloc(&rv, first_word);
      }
      return rv;
    }

    unsigned evtype = EventsEncoder::decode_type(first_word);
    rv.type = evtype;

    switch (evtype) {
    case EventsEncoder::kEventRealloc:
      curr_thread->consume_realloc(&rv, first_word, read_varint());
      break;
    case EventsEncoder::kEventMemalign:
      curr_thread->consume_memalign(&rv, first_word, read_varint());
      break;
    case EventsEncoder::kEventTok: {
      uint64_t second_word = read_varint();
      uint64_t third_word = read_varint();

      EventsEncoder::decode_token(&rv.tok, first_word, second_word, third_word);

      auto it = thread_states.find(rv.tok.thread_id);
      assert(it != thread_states.end());
      ThreadState *thread_state = &it->second;

      assert(thread_state == curr_thread);

      thread_state->last_cpu = rv.buf.cpu;
      thread_state->last_ts = rv.buf.ts;

      thread_state->consume_tok(rv.tok);
      break;
    }
    case EventsEncoder::kEventDeath: {
      EventsEncoder::decode_death(&rv.death, first_word, read_varint());

      auto it = thread_states.find(rv.death.thread_id);
      assert(it != thread_states.end());
      thread_states.erase(it);
      break;
    }
    case EventsEncoder::kEventBuf: {
      uint64_t second_word = read_varint();
      uint64_t third_word = read_varint();

      EventsEncoder::decode_buffer(&rv.buf, first_word, second_word, third_word);

      auto p = thread_states.emplace(rv.buf.thread_id,
                                     ThreadState(rv.buf.thread_id));
      rv.new_thread = p.second;
      auto it = p.first;
      curr_thread = &it->second;
      assert(!have_buf);
      last_buf = rv.buf;
      buf_size_left = rv.buf.size;
      have_buf = true;
      break;
    }
    case EventsEncoder::kEventEnd:
      rv.type = evtype;
      done = true;
      break;
    default:
      printf("unknown type: %u\n", evtype);
      abort();
    }
    return rv;
  }
};

char event_stream_space[sizeof(EventsStream)] __attribute__((aligned(4096)));

void __attribute__((weak)) fake_consume(const EventUnion &u) {
  // __asm__ __volatile__("" : : : "memory");
}

struct space_tree {
  typedef std::vector<uint64_t> bvector;

  bvector level0;
  bvector level1;
  bvector level2;
  bvector level3;

  bvector level1_empty;
  bvector level2_empty;
  bvector level3_empty;

  static unsigned bsf(uint64_t p) {
    assert(p != 0);
    return __builtin_ffsll(p) - 1;
  }

  bool find_set_bit(const bvector &v, uint64_t *pos) {
    for (auto i = v.begin(); i != v.end(); i++) {
      uint64_t val = *i;
      if (val != 0) {
        *pos = (i - v.begin())*64 + bsf(val);
        return true;
      }
    }
    return false;
  }

  template <bool bval>
  bool __set_bit(uint64_t pos, bvector &v, uint64_t *val_p) {
    uint64_t val = v[pos / 64];
    if (!bval) {
      *val_p = val;
      val &= ~(1ULL << (pos % 64));
      v[pos / 64] = val;
    } else {
      uint64_t new_val = val | (1ULL << (pos % 64));
      v[pos / 64] = new_val;
      *val_p = new_val;
    }
    return (val == 0);
  }

  template <bool bval>
  void set_bit(uint64_t pos) {
    uint64_t new_val;
    if (!__set_bit<bval>(pos, level0, &new_val)) {
      if (new_val == (uint64_t)(int64_t)-1) {
        pos /= 64;
        if (!__set_bit<bval>(pos, level1_empty, &new_val)) {
          return;
        }
        pos /= 64;
        if (!__set_bit<bval>(pos, level2_empty, &new_val)) {
          return;
        }
        pos /= 64;
        __set_bit<bval>(pos, level3_empty, &new_val);
      }
      return;
    }
    pos /= 64;
    if (!__set_bit<bval>(pos, level1, &new_val)) {
      return;
    }
    pos /= 64;
    if (!__set_bit<bval>(pos, level2, &new_val)) {
      return;
    }
    pos /= 64;
    __set_bit<bval>(pos, level3, &new_val);
  }

  struct thread_cache {
    uint64_t base;
    uint64_t bits;
  };

  uint64_t allocate_bunch(thread_cache *p) {
    uint64_t pos3;
    bool ok = find_set_bit(level3_empty, &pos3);
    if (!ok) {
      return allocate_new_id();
    }

    unsigned p2 = bsf(level2_empty[pos3]);
    uint64_t pos2 = pos3 * 64 + p2;
    unsigned p1 = bsf(level1_empty[pos2]);
    uint64_t pos1 = pos2 * 64 + p1;
    assert(level0[pos1] == (uint64_t)(int64_t)-1);
    p->bits = (uint64_t)(int64_t)-2;
    p->base = pos1 * 64;
    return p->base;
  }

  uint64_t allocate_id(thread_cache *p) {
    if (p) {
      if (p->bits) {
        unsigned i = bsf(p->bits);
        p->bits ^= 1ULL << i;
        return p->base + i;
      }
      return allocate_bunch(p);
    }

    uint64_t pos3;
    bool ok = find_set_bit(level3, &pos3);
    if (!ok) {
      return allocate_new_id();
    }
    unsigned p2 = bsf(level2[pos3]);
    uint64_t pos2 = pos3 * 64 + p2;
    unsigned p1 = bsf(level1[pos2]);
    uint64_t pos1 = pos2 * 64 + p1;
    unsigned p0 = bsf(level0[pos1]);
    uint64_t pos0 = pos1 * 64 + p0;
    set_bit<false>(pos0);
    return pos0;
  }

  void free_id(uint64_t id) {
    set_bit<true>(id);
  }

  uint64_t allocate_new_id() {
    uint64_t idx = level0.size();
    level0.push_back(0);
    set_bit<true>(idx * 64);
    level0[idx] = (uint64_t)(int64_t)-2;
    return idx * 64;
  }
};

struct ThreadReplayState {
  EventUnion next_event;
  bool has_next = false;
  bool live = true;
  uint64_t next_at_counter = 0;
  std::deque<EventUnion> pending;

  bool add_event(const EventUnion &ev, uint64_t counter) {
    if (has_next) {
      pending.push_back(ev);
      return false;
    } else {
      next_event = ev;
      next_at_counter = counter;
      has_next = true;
      return true;
    }
  }
};

struct ReadyReplyStateGreater {
  bool operator()(ThreadReplayState *a, ThreadReplayState *b) const {
    assert(b->has_next);
    assert(a->has_next);
    // if (!b->has_next) {
    //   return false;
    // }
    // if (!a->has_next) {
    //   return true;
    // }
    return a->next_event.ts > b->next_event.ts;
  }
};

#if 0

#include <gperftools/malloc_extension.h>

void dump_heap_and_exit(void) {
  std::string sample;
  MallocExtension::instance()->GetHeapSample(&sample);
  FILE *f = fopen("heap_sample", "wb");
  fwrite(sample.data(), sample.size(), 1, f);
  fclose(f);
  asm volatile ("int $3; nop");
  exit(0);
}

#endif

struct ReplayMachine {
  std::deque<ThreadReplayState> states;
  std::unordered_map<uint64_t, ThreadReplayState *> pending_frees;
  std::unordered_set<uint64_t> allocated;
  std::unordered_set<uint64_t> dropped;
  space_tree ids_space;
  std::priority_queue<ThreadReplayState *,
                      std::vector<ThreadReplayState *>,
                      ReadyReplyStateGreater> ready_events;
  bool seen_end = false;
  time_t start;
  uint64_t count = 0;
  uint64_t total_read = 0;
  uint64_t dropped_count = 0;
  uint64_t dropped_allocations = 0;
  int live_threads_count = 0;

  EventsStream *str;
  ReplayMachine(EventsStream *str) : str(str) {
    start = time(nullptr);
  }

  uint64_t steps_back = 0;
  uint64_t steps_back_total = 0;
  uint64_t max_step_back = 0;
  uint64_t prev_ts = 0;

  uint64_t step_back_ts = 0;
  uint64_t step_back_skip_count = 0;

  EventUnion prev_ev = {};

  void consume_event(const EventUnion &ev) {
    uint64_t ts = ev.ts;
    if (ts < prev_ts) {
      uint64_t amount = prev_ts - ts;
      steps_back++;
      steps_back_total += amount;
      if (amount > max_step_back) {
        max_step_back = amount;
        printf("new max_step_back = %llu\n", (unsigned long long)amount);
        prev_ev.print();
        ev.print();
      }

      step_back_ts = prev_ts;
      // ev.print();
      // fflush(stdout);
      // asm volatile ("int $3; nop");
    } else if (ts < step_back_ts) {
      // asm volatile ("int $3; nop");
      step_back_skip_count++;
    } else {
      step_back_ts = ts;
    }
    prev_ts = ts;
    prev_ev = ev;

    if ((count % (128 << 20)) == 0) {
      time_t now = time(nullptr);
      double rate = (double)count / (now - start);
      if (now == start) {
        rate = 0;
      }
      printf("count = %llu (%g events/sec)\n",
             (unsigned long long)count, rate);
    }
    // if (ev.type != EventsEncoder::kEventFree) {
    //   ev.print();
    // }
    // if (count >= 200000000LLU) {
    //   dump_heap_and_exit();
    // }
  }

  bool is_interesting_event(const EventUnion &ev) {
    switch (ev.type) {
    case EventsEncoder::kEventMalloc:
    case EventsEncoder::kEventFree:
    case EventsEncoder::kEventMemalign:
    case EventsEncoder::kEventRealloc:
      return true;
    }
    return false;
  }

  void maybe_ready_event(const EventUnion &ev, ThreadReplayState *t) {
    switch (ev.type) {
    case EventsEncoder::kEventMalloc:
    case EventsEncoder::kEventMemalign:
      ready_events.push(t);
      break;
    case EventsEncoder::kEventFree:
      if (allocated.count(ev.free.token)) {
        ready_events.push(t);
      } else {
        pending_frees.insert(std::make_pair(ev.free.token, t));
      }
      break;
    case EventsEncoder::kEventRealloc:
      if (allocated.count(ev.realloc.old_token)) {
        ready_events.push(t);
      } else {
        pending_frees.emplace(ev.realloc.old_token, t);
      }
    }
  }

  bool fetch_next_event() {
    if (seen_end) {
      return true;
    }
    for (int i = 128; i > 0; i--) {
      auto ev = str->next();
      if (ev.type == EventsEncoder::kEventEnd) {
        seen_end = true;
        return false;
      }
      if (ev.type == EventsEncoder::kEventDeath) {
        assert(states.size() > ev.death.thread_id);
        ThreadReplayState *t = &states[ev.death.thread_id];
        assert(t->live);
        t->live = false;
        if (!t->has_next) {
          live_threads_count--;
        }
        continue;
      }
      if (!is_interesting_event(ev)) {
        continue;
      }
      total_read++;
      uint64_t thread_id = ev.malloc.thread_id;
      if (thread_id >= states.size()) {
        live_threads_count += (thread_id + 1 - states.size());
        states.resize(thread_id+1);
      }
      ThreadReplayState *t = &states[thread_id];
      assert(t->live);
      bool is_next = t->add_event(ev, total_read);
      if (!is_next) {
        continue;
      }
      maybe_ready_event(ev, t);
    }
    return false;
  }

  void process_allocation(uint64_t token, EventUnion &ev) {
    auto pit = pending_frees.find(token);
    if (pit != pending_frees.end()) {
      ThreadReplayState *t = pit->second;
      pending_frees.erase(pit);
      ready_events.push(t);
    }
    bool inserted = allocated.insert(token).second;
    if (dropped.count(token) != 0) {
      printf("allocated previously dropped token:\n");
      ev.print();
      dropped_allocations++;
    }
    assert(inserted);
    // printf("allocated token %llu\n", (unsigned long long)token);
  }

  void process_free(uint64_t token) {
    auto it = allocated.find(token);
    assert(it != allocated.end());
    allocated.erase(it);
    // printf("free token %llu\n", (unsigned long long)token);
  }

  void advance_thread_state(ThreadReplayState *st) {
    if (st->pending.empty()) {
      st->has_next = false;
      if (!st->live) {
        live_threads_count--;
      }
    } else {
      EventUnion &ev = st->next_event;
      st->next_event = st->pending.front();
      st->pending.pop_front();
      maybe_ready_event(ev, st);
    }
  }

  void drop_pending_frees(void) {
    ThreadReplayState *worst;
    uint64_t earliest_counter = -1LL;
    for (auto pair : pending_frees) {
      auto t = pair.second->next_at_counter;
      if (t < earliest_counter) {
        worst = pair.second;
        earliest_counter = t;
      }
    }
    for (auto it = pending_frees.begin(); it != pending_frees.end(); it++) {
      auto st = it->second;
      if (st == worst) {
        printf("dropping pending free:\n");
        st->next_event.print();
        dropped.insert(st->next_event.free.token);
        pending_frees.erase(it);
        advance_thread_state(st);
        dropped_count++;
        count++;
        return;
      }
    }
  }

  static const long long kDropThreshold = 25000000LL;
  static const long long kMinPending =    15000000LL;

  void loop() {
    bool seen_end = false;
    while (true) {
      if (seen_end) {
        if (ready_events.empty()) {
          break;
        }
      } else if (ready_events.empty() || (total_read - count < kMinPending)) {
        if (fetch_next_event()) {
          seen_end = true;
          continue;
        }
        if (total_read - count >= kDropThreshold) {
          drop_pending_frees();
        }
        continue;
      }

      assert(ready_events.size() > 0);
      ThreadReplayState *st = ready_events.top();
      ready_events.pop();
      assert(st->has_next);

      EventUnion &ev = st->next_event;

      count++;
      consume_event(ev);

      switch (ev.type) {
      case EventsEncoder::kEventMalloc:
        process_allocation(ev.malloc.token, ev);
        break;
      case EventsEncoder::kEventFree:
        process_free(ev.free.token);
        break;
      case EventsEncoder::kEventMemalign:
        process_allocation(ev.memalign.token, ev);
        break;
      case EventsEncoder::kEventRealloc:
        process_free(ev.realloc.old_token);
        process_allocation(ev.realloc.new_token, ev);
        break;
      default:
        abort();
      }

      advance_thread_state(st);
    }
    printf("left_allocated = %lld\n", (long long)(allocated.size()));
    printf("dropped_count = %llu\n", (unsigned long long)dropped_count);
    printf("dropped_allocations = %llu\n", (unsigned long long)dropped_allocations);
    printf("left_pending_free = %lld\n", (long long)(pending_frees.size()));
    printf("count = %lld\n", (long long)count);
    printf("\n");
    printf("steps_back = %llu\n", (unsigned long long)steps_back);
    printf("steps_back_total = %llu\n", (unsigned long long)steps_back_total);
    printf("step_back_skip_count = %llu\n", (unsigned long long)step_back_skip_count);
    printf("max_steps_back = %llu\n", (unsigned long long)max_step_back);
  }
};

int main(int argc, char **argv)
{
  printf("sizeof(EventUnion) = %d\n", (int)sizeof(EventUnion));
  {
    void *p;
    uint64_t l = 1400ULL << 20;
    int rv = posix_memalign(&p, 2 << 20, l);
    if (rv != 0) {
      abort();
    }
    rv = madvise(p, l, MADV_HUGEPAGE);
    if (rv != 0) {
      perror("madvise");
      abort();
    }
    memset(p, 0xff, l);
    asm volatile ("" : : : "memory");
    free(p);
  }
  if (argc > 1) {
    int rv = open(argv[1], O_RDONLY);
    if (rv < 0) {
      perror("open");
    }
    dup2(rv, 0);
    close(rv);
  }

  signal(SIGINT, [](int dummy) {
      exit(0);
    });

  EventsStream::reader_fn_t reader = [](void *ptr, size_t sz) -> int {
    return read(0, ptr, sz);
  };
  EventsStream *str = new (&event_stream_space) EventsStream(reader);
  ReplayMachine r(str);
  r.loop();
  return 0;
}
