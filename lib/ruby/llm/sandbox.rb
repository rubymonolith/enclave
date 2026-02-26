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

      def expose(mod)
        @tool_context.extend(mod)
        mod.instance_methods(false).each do |name|
          _define_function(name.to_s)
        end
        self
      end
    end
  end
end
