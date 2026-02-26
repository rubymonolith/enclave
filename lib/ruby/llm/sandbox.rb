require_relative "sandbox/version"
require_relative "sandbox/result"
require_relative "sandbox/tool"
require_relative "ruby_llm_sandbox"

module Ruby
  module LLM
    class Sandbox
      def initialize(tools: nil)
        @tool_context = Object.new
        _init
        expose(tools) if tools
      end

      def self.open(tools: nil)
        sandbox = new(tools: tools)
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
        prompt = "sandbox> "

        puts "Ruby LLM Sandbox REPL (#{RUBY_ENGINE} host, mruby sandbox)"
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
  end
end
