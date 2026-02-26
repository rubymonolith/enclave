require_relative "lib/ruby/llm/sandbox/version"

Gem::Specification.new do |spec|
  spec.name = "ruby-llm-sandbox"
  spec.version = Ruby::LLM::Sandbox::VERSION
  spec.authors = ["Brad Gessler"]
  spec.summary = "Sandboxed MRuby execution environment for AI agents"
  spec.description = "Embeds MRuby as an in-process sandboxed Ruby execution environment. " \
                     "Provides a stateful REPL interface for AI agents to send code and get back results."
  spec.homepage = "https://github.com/bradgessler/ruby-llm-sandbox"
  spec.license = "MIT"
  spec.required_ruby_version = ">= 3.1.0"

  spec.files = Dir[
    "lib/**/*.rb",
    "ext/**/*.{c,h,rb}",
    "Rakefile",
    "README.md",
    "LICENSE"
  ]
  spec.require_paths = ["lib"]
  spec.extensions = ["ext/ruby_llm_sandbox/extconf.rb"]

  spec.add_dependency "rake-compiler", "~> 1.2"

  spec.add_development_dependency "rspec", "~> 3.0"
  spec.add_development_dependency "rake", "~> 13.0"
end
