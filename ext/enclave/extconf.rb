require "mkmf"

# Paths — must be absolute since rake-compiler changes cwd
ext_dir = File.expand_path(File.dirname(__FILE__))
mruby_dir = File.join(ext_dir, "mruby")
build_config = File.join(ext_dir, "sandbox_build_config.rb")
mruby_build_dir = File.join(mruby_dir, "build", "host")

# Build mruby from source
unless File.exist?(File.join(mruby_build_dir, "lib", "libmruby.a"))
  puts "Building mruby..."
  system("cd #{mruby_dir} && MRUBY_CONFIG=#{build_config} rake -f #{mruby_dir}/Rakefile -j1") || abort("mruby build failed")
end

# mruby headers (only used by sandbox_core.c, not enclave.c)
$INCFLAGS << " -I#{File.join(mruby_dir, 'include')}"
$INCFLAGS << " -I#{File.join(mruby_build_dir, 'include')}"

# Include the ext dir for sandbox_core.h
$INCFLAGS << " -I#{ext_dir}"

# Must match the defines used when building mruby
$CFLAGS << " -DMRB_USE_DEBUG_HOOK"

# Both .c files in the extension directory
$srcs = [
  File.join(ext_dir, "enclave.c"),
  File.join(ext_dir, "sandbox_core.c")
]

# Link libmruby.a statically
$LDFLAGS << " -L#{File.join(mruby_build_dir, 'lib')}"
$libs << " #{File.join(mruby_build_dir, 'lib', 'libmruby.a')}"
$libs << " -lm"

create_makefile("enclave/enclave")
