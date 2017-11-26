#!/usr/bin/ruby

require 'pp'
require 'capn_proto'

module Replay
  extend CapnProto::SchemaLoader
  load_schema 'replay.capnp'
end

include Replay

# Instruction = Struct.new(:type, :reg, :size)

# class Instruction
#   def self.read(io)
#     typereg, size = io.read(16).unpack("QQ")
#     # reg = typereg & -(1 << 56)
#     # typeraw = typereg >> 56
#     typeraw = typereg & 0xff
#     reg = typereg >> 8
#     type = case typeraw
#            when 0
#              :malloc
#            when 1
#              :free
#            else
#              raise "unknown instruction type: #{typeraw}"
#            end
#     self.new(type, reg, size)
#   end
# end

# ChunkInfo = Struct.new(:thread_count)

# class ChunkInfo
#   def self.read(io)
#     thread_count = io.read(8).unpack("Q").first
#     self.new(thread_count)
#   end
# end

# ThreadInfo = Struct.new(:thread_id, :live, :instructions_count, :instructions)

# class ThreadInfo
#   def self.read(io)
#     thread_id, liveraw, instructions_count = io.read(16).unpack("QCxxxL")
#     live = (liveraw != 0)
#     pp [thread_id, live, instructions_count]
#     instructions = instructions_count.times.map do
#       Instruction.read(io)
#     end
#     self.new(thread_id, live, instructions_count, instructions)
#   end
# end

while not STDIN.eof?
  chunk = Replay::Batch.read_packed_from(STDIN)
  chunk.threads.each do |ti|
    pp ti
  end
end
