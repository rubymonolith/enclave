require "rake/extensiontask"
require "rspec/core/rake_task"

Rake::ExtensionTask.new("ruby_llm_sandbox") do |ext|
  ext.lib_dir = "lib/ruby/llm"
end

RSpec::Core::RakeTask.new(:spec)

task default: [:compile, :spec]
