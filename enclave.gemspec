require_relative "lib/enclave/version"

Gem::Specification.new do |spec|
  spec.name = "enclave"
  spec.version = Enclave::VERSION
  spec.authors = ["Brad Gessler"]
  spec.summary = "Sandboxed Ruby for AI agents"
  spec.description = "Embeds MRuby as an in-process sandboxed Ruby execution environment. " \
                     "Provides a stateful REPL interface for AI agents to send code and get back results."
  spec.homepage = "https://github.com/rubymonolith/enclave"
  spec.license = "MIT"
  spec.required_ruby_version = ">= 3.1.0"

  spec.files = Dir[
    "lib/**/*.rb",
    "ext/**/*.{c,h,rb}",
    "ext/enclave/mruby/{Rakefile,Makefile}",
    "ext/enclave/mruby/{include,src,mrblib,mrbgems}/**/*",
    "ext/enclave/mruby/{build_config,tasks,lib}/**/*",
    "ext/enclave/mruby/tools/lrama/**/*",
    "Rakefile",
    "README.md",
    "LICENSE"
  ]
  spec.require_paths = ["lib"]
  spec.extensions = ["ext/enclave/extconf.rb"]

  spec.add_dependency "rake-compiler", "~> 1.2"

  spec.add_development_dependency "rspec", "~> 3.0"
  spec.add_development_dependency "rake", "~> 13.0"
end
