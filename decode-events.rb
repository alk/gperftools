#!/usr/bin/ruby

require 'stringio'

LOG_VARINT = false

class Class
  def def_custom_attrs *attrs
    class_eval <<HERE
attr_reader #{attrs.map {|a| ":#{a}"}.join(', ')}
def initialize(#{attrs.join(' ,')})
  @#{attrs.join(', @')} = #{attrs.join(', ')}
end
HERE
  end
end

module Events
  class Malloc
    def_custom_attrs :thread_id, :tok, :size
  end
  class Free
    def_custom_attrs :thread_id, :tok
  end
  class Realloc
    def_custom_attrs :thread_id, :tok, :size, :old_tok
  end
  class Memalign
    def_custom_attrs :thread_id, :tok, :size, :align
  end
  class Tok
    def_custom_attrs :thread_id, :ts, :cpu, :token_base
  end
  class Death
    def_custom_attrs :thread_id, :ts, :cpu
  end
  class Buf
    def_custom_attrs :thread_id, :ts, :cpu, :size
  end
  class End
  end
end

module Helpers
  def leading_zeros_slow(a)
    return nil if a == 0
    c = 0
    while (a & 1) == 0
      c += 1
      a = a >> 1
    end
    c
  end
  module_function :leading_zeros_slow

  def unzigzag(i)
    sign = i & 1
    (i >> 1) ^ -sign
  end
  module_function :unzigzag
end
raise unless Helpers.leading_zeros_slow(0x9) == 0
raise unless Helpers.leading_zeros_slow(0x90) == 4

class EventsStream
  @@leading_zeros = [9] + (1..256).map {|i| Helpers.leading_zeros_slow(i)}
  raise unless @@leading_zeros[0x9] == 0
  raise unless @@leading_zeros[0x90] == 4

  def read_varint
    b = @io.getbyte()
    bits = @@leading_zeros[b]
    raise "b = #{b}" unless bits
    off = 8
    bits.times do
      b |= (@io.getbyte() << off)
      off += 8
    end
    rv = b >> (bits + 1)
    printf "got 0x%016x\n", rv if LOG_VARINT
    rv
  end

  class ThreadState
    attr_reader :thread_id
    def initialize(thread_id)
      @thread_id = thread_id
      @prev_size = 0
      @prev_token = 0
      @malloc_tok_seq = nil
    end
    def consume_malloc(event)
      sz = Helpers.unzigzag(event >> 3) + @prev_size
      @prev_size = sz
      sz = sz << 3

      tok = @malloc_tok_seq
      @malloc_tok_seq += 1
      Events::Malloc.new(@thread_id, tok, sz)
    end
    def consume_free(event)
      tok = Helpers.unzigzag(event >> 3) + @prev_token
      @prev_token = tok
      Events::Free.new(@thread_id, tok)
    end
    def consume_realloc(first_word, second_word)
      sz = Helpers.unzigzag(first_word >> 3) + @prev_size
      @prev_size = sz
      sz = sz << 3
      tok = @malloc_tok_seq
      @malloc_tok_seq += 1
      old_tok = Helpers.unzigzag(second_word) + @prev_token
      @prev_token = old_tok
      Events::Realloc.new(@thread_id, tok, sz, old_tok)
    end
    def consume_memalign(first_word, second_word)
      sz = Helpers.unzigzag(first_word >> 3) + @prev_size
      @prev_size = sz
      sz = sz << 3
      tok = @malloc_tok_seq
      @malloc_tok_seq += 1
      align = second_word
      Events::Memalign.new(@thread_id, tok, sz, align)
    end

    def consume_tok(thread_id, ts, cpu, new_base)
      raise [thread_id, ts, cpu, new_base].inspect unless thread_id == @thread_id
      @malloc_tok_seq = new_base
      Events::Tok.new(@thread_id, ts, cpu, new_base)
    end
  end

  def read_ts_and_cpu
    int1 = read_varint()
    ts = int1 & -1024
    cpu = int1 & 1023
    [ts, cpu]
  end

  def initialize(io)
    @thread_states = {}
    @io = io
    @curr_thread = nil
    @main_io = io
    @done = false
  end
  def eof?
    @done
  end

  def next
    raise if @done

    if @io.eof?
      raise if @main_io.eof?
      @curr_thread = nil
      @io = @main_io
    end

    first = read_varint()
    evtype = first & 7
    first_hi = first >> 3
    if evtype == 7
      first_hi = first >> 8
      evtype = first & 0xff
    end

    case evtype
    when 0 # Malloc
      return @curr_thread.consume_malloc(first)
    when 1 #Free
      return @curr_thread.consume_free(first)
    when 2 # Tok
      thread_id = first_hi
      ts, cpu = *read_ts_and_cpu()
      last = read_varint()
      return @thread_states[thread_id].consume_tok(thread_id, ts, cpu, last)
    when 3 # Buf
      raise unless @io = @main_io
      thread_id = first_hi
      ts, cpu = *read_ts_and_cpu()
      buf_size = read_varint()

      buf = @io.read(buf_size)
      @io = StringIO.new(buf)

      @curr_thread = @thread_states[thread_id]
      unless @curr_thread
        @curr_thread = @thread_states[thread_id] = ThreadState.new(thread_id)
      end

      return Events::Buf.new(thread_id, ts, cpu, buf_size)
    when 07 # Death. Yes, octal
      thread_id = first_hi
      ts, cpu = *read_ts_and_cpu()
      raise unless @thread_states[thread_id]
      @thread_states.delete thread_id
      return Events::Death.new(thread_id, ts, cpu)
    when 017 # End. Yes, octal
      @done = true
      return Events::End.new
    when 027 # Realloc
      return @curr_thread.consume_realloc(first, read_varint())
    when 037 # Memalign
      return @curr_thread.consume_memalign(first, read_varint())
    else
      raise "bad code: #{first}"
    end
  end
end

if LOG_VARINT
  str = STDIN.read(1024)
  puts(str.each_byte.map {|b| "0x%02x" % [b]}.each_slice(16).map {|row| row.join(', ')}.join(",\n"))
  s = EventsStream.new(StringIO.new(str))
  15.times do
    p s.next
  end
  exit
end

s = EventsStream.new(STDIN)

4096.times do
  p s.next
end

count = 4096
while not s.eof?
  evt = s.next
  count += 1
  # if count % 409600 == 0
  #   puts "count: #{count}; evt: #{evt.inspect}"
  # end
  p evt
end

puts
puts "count: #{count}"
