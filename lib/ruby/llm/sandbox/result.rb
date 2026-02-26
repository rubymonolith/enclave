module Ruby
  module LLM
    class Sandbox
      class Result
        attr_reader :value, :output, :error

        def initialize(value:, output:, error:)
          @value = value
          @output = output
          @error = error
        end

        def error?
          !@error.nil?
        end

        def to_s
          if error?
            "Error: #{@error}"
          elsif !@output.empty?
            "#{@output}=> #{@value}"
          else
            "=> #{@value}"
          end
        end

        def inspect
          "#<#{self.class} value=#{@value.inspect} output=#{@output.inspect} error=#{@error.inspect}>"
        end
      end
    end
  end
end
