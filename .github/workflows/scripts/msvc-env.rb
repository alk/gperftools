#!/usr/bin/ruby

VSWHERE = "#{ENV['ProgramFiles(x86)']}\\Microsoft Visual Studio\\Installer\\vswhere.exe"

VSPATH = `"#{VSWHERE}" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`.strip

if VSPATH.empty?
  raise "Unable to fetch visual studio installation path. VSWHERE=#{VSWHERE.inspect} VSPATH=#{VSPATH.inspect}"
end

unless File.directory? VSPATH
  raise "bogus VSPATH? VSPATH=#{VSPATH.inspect}"
end

require 'json'

# MSVC and windows stuff is sick. Or maybe I am not aware of
# how to do it nicer. So what happens below?
#
# Well, we need environment variables setup as if vcvars64.bat is run
# (the one that does "cmd with visual studio environment whatever
# stuff"). "Thanks" to madness of cmd and .bat files I see no other
# way than to create .bat file, that will "call" vcvars thingy and
# then invoke ruby which will then dump environment variables we
# need. Then "outer" script handles unmarshalling and caching this.

Dir.chdir(File.dirname(__FILE__)) do
  h = begin
        JSON.parse(IO.read("env-cache"))
      rescue
        nil
      end
  unless h
    File.open("msvc-env.bat", "w") do |f|
      f.write(<<HERE)
@call "#{File.join(VSPATH, 'VC/Auxiliary/Build/vcvars64.bat')}"
@ruby -rjson -e "puts('!'*32); puts ENV.to_hash.to_json"
HERE
    end
    output = `msvc-env.bat`
    h = JSON.parse(output.split('!'*32, 2).last)

    required_vars = %w[INCLUDE LIB VCINSTALLDIR]
    missing_vars = required_vars.reject { |var| h.key?(var) }

    unless missing_vars.empty?
      raise "ERROR: Environment capture succeeded but missing key MSVC variables: #{missing_vars.join(', ')}"
    end

    File.open("env-cache", "w") {|f| f << h.to_json }
  end
  ENV.update(h)
end

# We then exec cmake/ninja as directed with environment variables updated

exec(*ARGV)
