MRuby::Build.new do |conf|
  conf.toolchain :clang

  # Safe standard library — no IO, no sockets, no filesystem
  conf.gembox "stdlib"
  conf.gembox "stdlib-ext"
  conf.gembox "math"
  conf.gembox "metaprog"

  # print gem gives us Kernel#print and Kernel#p (we override __printstr__ equivalent)
  # NOT included: mruby-io (File, Socket, Dir), mruby-bin-* (executables)

  # Build as static library only — we link into the Ruby C extension
  conf.cc.flags << "-fPIC"
end
