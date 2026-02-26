module Ruby
  module LLM
    class Sandbox
      module Tool
        def self.definition
          {
            type: "function",
            function: {
              name: "eval_ruby",
              description: "Evaluate Ruby code in a sandboxed MRuby environment. " \
                           "State persists between calls. Use instance variables (@x) " \
                           "and method definitions to build up context. " \
                           "Available: Arrays, Hashes, Strings, Math, Structs, Enumerators. " \
                           "Not available: File IO, network, system commands, require.",
              parameters: {
                type: "object",
                properties: {
                  code: {
                    type: "string",
                    description: "Ruby code to evaluate"
                  }
                },
                required: ["code"]
              }
            }
          }
        end

        def self.call(sandbox, code:)
          result = sandbox.eval(code)
          if result.error?
            "Error: #{result.error}"
          elsif !result.output.empty?
            "#{result.output}=> #{result.value}"
          else
            "=> #{result.value}"
          end
        end
      end
    end
  end
end
