require "bundler/gem_tasks"
require "rake/extensiontask"
require "rspec/core/rake_task"

task :init_submodules do
  mruby_dir = File.expand_path("ext/enclave/mruby", __dir__)
  unless File.exist?(File.join(mruby_dir, "Rakefile"))
    puts "Initializing mruby submodule..."
    sh "git submodule update --init"
  end
end

task compile: :init_submodules

Rake::ExtensionTask.new("enclave") do |ext|
  ext.lib_dir = "lib/enclave"
end

RSpec::Core::RakeTask.new(:spec)

task default: [:compile, :spec]

desc "Set up development environment"
task setup: :init_submodules do
  sh "bundle install"
  Rake::Task[:compile].invoke
  Rake::Task[:spec].invoke
end

namespace :mruby do
  desc "Update mruby to latest and rebuild"
  task :update do
    sh "git -C ext/enclave/mruby fetch origin"
    sh "git -C ext/enclave/mruby checkout origin/master"
    Rake::Task["mruby:clean"].invoke
    puts "mruby updated. Run `rake compile` to rebuild."
  end

  desc "Clean mruby build artifacts"
  task :clean do
    rm_rf "ext/enclave/mruby/build"
    rm_rf "tmp"
    rm_f "lib/enclave/enclave.bundle"
    rm_f "lib/enclave/enclave.so"
    puts "mruby build artifacts cleaned."
  end
end
