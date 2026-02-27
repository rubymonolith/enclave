require "rake/extensiontask"
require "rspec/core/rake_task"

Rake::ExtensionTask.new("enclave") do |ext|
  ext.lib_dir = "lib/enclave"
end

RSpec::Core::RakeTask.new(:spec)

task default: [:compile, :spec]
