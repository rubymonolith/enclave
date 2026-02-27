require_relative "enclave/version"
require_relative "enclave/result"
require_relative "enclave/tool"
require_relative "enclave/enclave"

class Enclave
  class << self
    attr_accessor :timeout, :memory_limit
  end

  attr_reader :timeout, :memory_limit

  def initialize(tools: nil, timeout: self.class.timeout, memory_limit: self.class.memory_limit)
    @tool_context = Object.new
    @timeout = timeout
    @memory_limit = memory_limit
    _init(@timeout, @memory_limit)
    expose(tools) if tools
  end

  def self.open(tools: nil, timeout: self.timeout, memory_limit: self.memory_limit)
    sandbox = new(tools: tools, timeout: timeout, memory_limit: memory_limit)
    begin
      yield sandbox
    ensure
      sandbox.close
    end
  end

  def eval(code)
    value, output, error = _eval(code)
    Result.new(value: value, output: output, error: error)
  end

  def repl
    require "readline"
    buf = ""
    prompt = "enclave> "

    puts "Enclave REPL (#{RUBY_ENGINE} host, mruby sandbox)"
    puts "Type 'exit' or Ctrl-D to quit.\n\n"

    while (line = Readline.readline(buf.empty? ? prompt : "     .. ", true))
      break if buf.empty? && line.strip == "exit"

      buf << line << "\n"
      result = eval(buf)

      if result.error? && result.error.match?(/SyntaxError.*unexpected.*\$end|unexpected end of file/i)
        next # incomplete input, keep reading
      end

      print result.output unless result.output.empty?
      if result.error?
        puts "Error: #{result.error}"
      else
        puts "=> #{result.value}"
      end
      buf = ""
    end

    puts "\n" if line.nil? # clean newline on Ctrl-D
  end

  def expose(obj)
    case obj
    when Module
      @tool_context.extend(obj)
      obj.instance_methods(false).each do |name|
        _define_function(name.to_s)
      end
    else
      obj.public_methods(false).each do |name|
        target = obj
        @tool_context.define_singleton_method(name) { |*args| target.public_send(name, *args) }
        _define_function(name.to_s)
      end
    end
    self
  end
end
