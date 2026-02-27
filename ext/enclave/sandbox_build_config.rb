MRuby::Build.new do |conf|
  conf.toolchain :clang

  # Safe standard library — no IO, no sockets, no filesystem
  conf.gembox "stdlib"
  conf.gembox "stdlib-ext"
  conf.gembox "math"
  conf.gembox "metaprog"

  # print gem gives us Kernel#print and Kernel#p (we override __printstr__ equivalent)
  # NOT included: mruby-io (File, Socket, Dir), mruby-bin-* (executables)

  # Enable debug hook for code_fetch_hook (used for timeout)
  conf.cc.defines << "MRB_USE_DEBUG_HOOK"

  # Build as static library only — we link into the Ruby C extension
  conf.cc.flags << "-fPIC"
end
