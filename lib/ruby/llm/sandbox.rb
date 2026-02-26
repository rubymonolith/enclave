require_relative "sandbox/version"
require_relative "sandbox/result"
require_relative "sandbox/tool"
require_relative "ruby_llm_sandbox"

module Ruby
  module LLM
    class Sandbox
      def self.open
        sandbox = new
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
    end
  end
end
