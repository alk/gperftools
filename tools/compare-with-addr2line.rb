#!/usr/bin/ruby

if ARGV.size != 1
  puts "I take single argument: path to ELF binary or shared library to compare"
  exit 1
end

binary = ARGV[0]

unless File.file?(binary) && File.readable?(binary)
  puts "#{binary} needs to be readable file"
  exit 1
end

RANGES = []
NM_NAMES = {}

(`nm -S -n "#{binary}" | grep -i ' t '`).each_line do |ln|
  ln.chomp!
  next if ln =~ /\A[0-9a-f]+\s+(t|T)\s+\S+\z/ # entry without size
  raise "bad line: #{ln.inspect}" unless ln =~ /\A([0-9a-f]+)\s+([0-9a-f]+)\s+(t|T)\s+(\S+)\z/
  addr, size, name = $1.to_i(16), $2.to_i(16), $4

  (NM_NAMES[addr] ||= []) << name
  RANGES << [addr, addr + size]
end


min_addr = RANGES.first.first
max_addr = RANGES[-1].first + RANGES[-1].last

def in_ranges?(addr)
  l = 0
  r = RANGES.size
  return false if l == r
  while l + 1 < r
    x = (l + r) / 2
    if RANGES[x].first <= addr
      l = x
    else
      r = x
    end
  end
  return l if RANGES[l].first <= addr && addr < RANGES[l].last
end

# p [min_addr.to_s(16), max_addr.to_s(16)]

minipprof = File.join(File.dirname(__FILE__), "../minipprof")
addr2line = ENV['ADDR2LINE'] || "addr2line"

srand(0)
sample = {}
while sample.size < 10000
  candidate = min_addr + rand(max_addr - min_addr)
  if !sample.has_key?(candidate) && in_ranges?(candidate)
    sample[candidate] = true
    # puts "adding sample: #{candidate.to_s(16)}"
  end
end

sample = sample.keys

address_args = sample.map {|e| e.to_s(16)}.join(' ')

puts "Sample addresses are: #{address_args}"

DEMANGLE = false

a2l_result = `"#{addr2line}" #{'-C ' if DEMANGLE}-f -i -a -e "#{binary}" #{address_args}`.each_line.map(&:chomp)
mpp_result = `"#{minipprof}" #{'--no-demangle' unless DEMANGLE} "#{binary}" #{address_args}`.each_line.map(&:chomp)

# puts "a2l_result:"
# puts a2l_result
# puts
# puts
# puts "mpp_result:"
# puts mpp_result
# puts
# puts

SymLoc = Struct.new(:function, :filename, :lineno)

class ResultsEnumerator
  def initialize(lines)
    @lines = lines
  end

  def must_addr(s)
    raise "need address got: #{s}" unless s =~ /\A0x[0-9a-f]+\z/
    s.to_i(16)
  end

  # [pc, [SymLoc...]] or nil when at the end
  def next
    return nil if @lines.empty?

    pc = must_addr(@lines.shift)
    locs = []
    raise "address in function name line?: #{@lines.first}" if @lines.first =~ /\A0x/
    while true
      function = @lines.shift
      filename, loc = @lines.shift.split(":", 2)
      if loc =~ /\s+\(discriminator/
        loc = $`
      end
      # puts " loc: #{function}, #{filename}, #{loc}"
      locs << SymLoc.new(function, filename, loc)
      break if @lines.empty? || @lines.first =~ /\A0x/
    end

    return [pc, locs]
  end

  def has_next?; not @lines.empty?; end
end


a2l_e = ResultsEnumerator.new(a2l_result)
mpp_e = ResultsEnumerator.new(mpp_result)

SKIP_FN_COMPARE = true
SKIP_FILE_COMPARE = false

was_bad = false
mismatches = []

def compare_paths(a2l, mpp)
  # addr2line seems to be giving full paths, while libbacktrace gives
  # me what looks like original path name passed to compiler (so,
  # usually relative)
  return true if a2l.end_with? mpp
  if mpp.start_with? "./"
    return a2l.end_with?(mpp[2..-1])
  end
  return false
end

def compare_and_print(lpc, l, rpc, r)
  is_bad = false

  printf("pc: 0x%08x vs 0x%08x\n", lpc, rpc)
  if lpc != rpc
    puts "PC mismatch!"
  end
  range_idx = in_ranges? lpc
  raise unless range_idx
  puts "NM names: #{NM_NAMES[RANGES[range_idx].first].join(' ')}"
  if l.size != r.size
    puts "location sizes mismatch #{l.size} vs #{r.size}"
  end
  [l.size, r.size].max.times do |i|
    lloc = l[i]
    is_bad ||= lloc.nil?
    if lloc
      printf("a2l %d: %s %s:%s\n", i, lloc.function, lloc.filename, lloc.lineno)
    end
    rloc = r[i]
    is_bad ||= rloc.nil?
    if rloc
      printf("mpp %d: %s %s:%s\n", i, rloc.function, rloc.filename, rloc.lineno)
    end
    if lloc && rloc
      badfn = (lloc.function != rloc.function)
      badfn = false if SKIP_FN_COMPARE
      badpath = !compare_paths(lloc.filename, rloc.filename)
      badline = (lloc.lineno != rloc.lineno)
      badpath = badline = false if SKIP_FILE_COMPARE
      if badfn || badpath || badline
        is_bad = true
        puts "^^^^^^ Mismatch!!!! function: #{badfn}, path: #{badpath}, line: #{badline}"
      end
    end
  end

  is_bad
end

while a2l_e.has_next?
  lpc, l = a2l_e.next
  raise "Ran out of results on addr2line side" unless mpp_e.has_next?
  rpc, r = mpp_e.next

  is_bad = compare_and_print(lpc, l, rpc, r)

  if is_bad
    was_bad = true
    mismatches << [lpc, l, rpc, r]
  end
end

exit(0) if !was_bad

puts "\nMismatches:"
mismatches.each do |(lpc, l, rpc, r)|
  compare_and_print(lpc, l, rpc, r)
  puts "------"
end

exit(1)

