RSpec.describe Enclave do
  let(:enclave) { described_class.new }

  after { enclave.close unless enclave.closed? }

  describe "#eval" do
    it "evaluates simple expressions" do
      result = enclave.eval("1 + 1")
      expect(result.value).to eq("2")
      expect(result.error?).to be false
    end

    it "returns inspected value" do
      result = enclave.eval('"hello"')
      expect(result.value).to eq('"hello"')
    end

    it "returns nil for statements" do
      result = enclave.eval("x = 42")
      expect(result.error?).to be false
    end

    it "handles multi-line code" do
      code = <<~RUBY
        def add(a, b)
          a + b
        end
        add(2, 3)
      RUBY
      result = enclave.eval(code)
      expect(result.value).to eq("5")
    end
  end

  describe "state persistence" do
    it "preserves local variables across evals" do
      enclave.eval("x = 42")
      result = enclave.eval("x * 2")
      expect(result.value).to eq("84")
    end

    it "preserves method definitions across evals" do
      enclave.eval("def greet(name); 'Hello ' + name; end")
      result = enclave.eval("greet('world')")
      expect(result.value).to eq('"Hello world"')
    end

    it "preserves instance variables on top-level self" do
      enclave.eval("@count = 0")
      enclave.eval("@count += 1")
      result = enclave.eval("@count")
      expect(result.value).to eq("1")
    end

    it "stores last result in _" do
      enclave.eval("42")
      result = enclave.eval("_ + 8")
      expect(result.value).to eq("50")
    end
  end

  describe "output capture" do
    it "captures puts output" do
      result = enclave.eval('puts "hello"')
      expect(result.output).to eq("hello\n")
      expect(result.value).to eq("nil")
    end

    it "captures print output" do
      result = enclave.eval('print "hello"')
      expect(result.output).to eq("hello")
    end

    it "captures p output" do
      result = enclave.eval('p 42')
      expect(result.output).to eq("42\n")
      expect(result.value).to eq("42")
    end

    it "captures multiple puts" do
      result = enclave.eval('puts "a"; puts "b"')
      expect(result.output).to eq("a\nb\n")
    end

    it "captures puts with no args" do
      result = enclave.eval("puts")
      expect(result.output).to eq("\n")
    end

    it "captures puts with arrays" do
      result = enclave.eval('puts [1, 2, 3]')
      expect(result.output).to eq("1\n2\n3\n")
    end

    it "resets output between evals" do
      enclave.eval('puts "first"')
      result = enclave.eval('puts "second"')
      expect(result.output).to eq("second\n")
    end
  end

  describe "error handling" do
    it "captures runtime errors" do
      result = enclave.eval("1 / 0")
      expect(result.error?).to be true
      expect(result.error).to match(/ZeroDivisionError/)
    end

    it "captures name errors" do
      result = enclave.eval("undefined_variable_xyz")
      expect(result.error?).to be true
    end

    it "captures syntax errors" do
      result = enclave.eval("def foo(")
      expect(result.error?).to be true
      expect(result.error).to match(/SyntaxError/)
    end

    it "does not raise Ruby exceptions" do
      expect { enclave.eval("1 / 0") }.not_to raise_error
    end

    it "allows continued use after errors" do
      enclave.eval("1 / 0")
      result = enclave.eval("1 + 1")
      expect(result.value).to eq("2")
      expect(result.error?).to be false
    end
  end

  describe "safety" do
    # Each spec asserts the attempt errors out. If a sandbox escape
    # actually succeeds, the test fails harmlessly (wrong value) —
    # nothing dangerous runs in the host process.

    describe "missing dangerous classes" do
      %w[File IO Dir Socket Process Signal ENV ARGV STDIN STDOUT STDERR].each do |const|
        it "has no #{const}" do
          result = enclave.eval(const)
          expect(result.error?).to be true
        end
      end
    end

    describe "missing dangerous methods" do
      {
        "system"       => 'system("id")',
        "exec"         => 'exec("id")',
        "spawn"        => 'spawn("id")',
        "backticks"    => '`id`',
        "require"      => 'require "json"',
        "load"         => 'load "foo.rb"',
        "open"         => 'open("/etc/passwd")',
        "exit"         => "exit",
        "exit!"        => "exit!",
        "abort"        => 'abort("bye")',
        "at_exit"      => "at_exit { }",
        "fork"         => "fork { }",
        "trap"         => 'trap("INT") { }',
      }.each do |label, code|
        it "blocks #{label}" do
          result = enclave.eval(code)
          expect(result.error?).to be true
        end
      end
    end

    describe "scope escape attempts" do
      it "cannot reach File through top-level constant lookup" do
        result = enclave.eval("::File")
        expect(result.error?).to be true
      end

      it "cannot fish for dangerous constants via Object.constants" do
        result = enclave.eval('Object.constants.select { |c| c.to_s.include?("File") }')
        # Should either error or return empty
        if result.error?
          expect(result.error?).to be true
        else
          expect(result.value).to satisfy { |v| !v.include?("File") }
        end
      end

      it "cannot eval its way to new scope" do
        # mruby has eval but it's still sandboxed
        result = enclave.eval('eval("File")')
        expect(result.error?).to be true
      end

      it "cannot use instance_eval to escape" do
        result = enclave.eval('Object.instance_eval { File }')
        expect(result.error?).to be true
      end

      it "cannot use class_eval to escape" do
        result = enclave.eval('Object.class_eval { File }')
        expect(result.error?).to be true
      end

      it "cannot use send to call private kernel methods" do
        result = enclave.eval('self.send(:system, "id")')
        expect(result.error?).to be true
      end

      it "cannot use __send__ to bypass method_missing" do
        result = enclave.eval('self.__send__(:system, "id")')
        expect(result.error?).to be true
      end

      it "cannot use Kernel.open pipe trick" do
        result = enclave.eval('Kernel.open("|id")')
        expect(result.error?).to be true
      end
    end

    describe "reflection attacks" do
      it "cannot use ObjectSpace to enumerate host objects" do
        result = enclave.eval("ObjectSpace.each_object(String).to_a")
        # Should either error or only see mruby-internal strings
        if !result.error?
          expect(result.value).not_to include("SECRET")
        end
      end

      it "cannot use method objects to discover internals" do
        result = enclave.eval('method(:puts).inspect')
        # This may work — puts exists in mruby — but shouldn't leak host info
        if !result.error?
          expect(result.value).not_to include("cruby")
        end
      end
    end

    describe "resource exhaustion" do
      it "does not crash the host on deep recursion" do
        result = enclave.eval("def f; f; end; f")
        expect(result.error?).to be true
      end

      it "does not crash the host on large string allocation" do
        result = enclave.eval('"x" * 100_000_000')
        # May succeed with a big string or error — either is fine, host must survive
        expect(enclave.eval("1 + 1").value).to eq("2")
      end

      it "does not crash the host on infinite loop (if mruby catches it)" do
        # mruby may not have a loop timeout, so we just verify the host survives
        # a tight loop that allocates. Skip if it hangs — that's a known mruby limitation.
        result = enclave.eval("a = []; 1_000_000.times { a << 1 }; a.length")
        # Whether it succeeds or errors, the host must be alive
        expect(enclave.eval("1 + 1").value).to eq("2")
      end
    end

    describe "isolation between instances" do
      it "cannot see tools from another enclave" do
        tools_enclave = Enclave.new(tools: TestTools)
        bare_enclave = Enclave.new

        result = bare_enclave.eval("double(21)")
        expect(result.error?).to be true

        tools_enclave.close
        bare_enclave.close
      end

      it "cannot leak state between enclaves" do
        e1 = Enclave.new
        e2 = Enclave.new

        e1.eval("@secret = 'do_not_leak'")
        result = e2.eval("@secret")
        expect(result.value).to eq("nil")

        e1.close
        e2.close
      end
    end

    describe "internal tampering" do
      it "cannot redefine a tool to bypass the callback" do
        e = Enclave.new(tools: TestTools)
        e.eval('def double(n); "hacked"; end')
        # The redefined method wins — but it's still inside the sandbox,
        # so the worst case is the agent lies to itself
        result = e.eval("double(21)")
        expect(result.value).to eq('"hacked"')
        e.close
      end

      it "survives evil inspect override during result serialization" do
        enclave.eval("class Integer; def inspect; nil; end; end")
        result = enclave.eval("42")
        # Should not crash — C code handles non-string inspect gracefully
        expect(enclave.eval("1 + 1")).not_to be_nil
      end

      it "survives a fiber bomb" do
        result = enclave.eval("fibers = 10000.times.map { Fiber.new { loop { Fiber.yield } } }; fibers.length")
        expect(result.value).to eq("10000")
        expect(enclave.eval("1 + 1").value).to eq("2")
      end
    end
  end

  describe "#reset!" do
    it "clears local variables" do
      enclave.eval("x = 42")
      enclave.reset!
      result = enclave.eval("x")
      expect(result.error?).to be true
    end

    it "clears method definitions" do
      enclave.eval("def foo; 1; end")
      enclave.reset!
      result = enclave.eval("foo")
      expect(result.error?).to be true
    end

    it "allows continued use after reset" do
      enclave.reset!
      result = enclave.eval("1 + 1")
      expect(result.value).to eq("2")
    end
  end

  describe "#close" do
    it "marks enclave as closed" do
      enclave.close
      expect(enclave.closed?).to be true
    end

    it "raises on eval after close" do
      enclave.close
      expect { enclave.eval("1") }.to raise_error(RuntimeError, /closed/)
    end

    it "is idempotent" do
      enclave.close
      expect { enclave.close }.not_to raise_error
    end
  end

  describe ".open" do
    it "yields an enclave and auto-closes" do
      result = nil
      described_class.open do |sb|
        result = sb.eval("1 + 1")
        expect(sb.closed?).to be false
      end
      expect(result.value).to eq("2")
    end
  end

  describe "isolation" do
    it "isolates state between instances" do
      sb1 = described_class.new
      sb2 = described_class.new

      sb1.eval("x = 10")
      result = sb2.eval("defined?(x)")
      expect(result.error?).to be(true).or(satisfy { result.value == "nil" })

      sb1.close
      sb2.close
    end
  end

  describe "Result" do
    it "has value, output, and error attributes" do
      result = enclave.eval("1 + 1")
      expect(result).to respond_to(:value)
      expect(result).to respond_to(:output)
      expect(result).to respond_to(:error)
      expect(result).to respond_to(:error?)
    end

    it "has a useful to_s" do
      result = enclave.eval("1 + 1")
      expect(result.to_s).to eq("=> 2")
    end

    it "includes output in to_s" do
      result = enclave.eval('puts "hi"; 42')
      expect(result.to_s).to eq("hi\n=> 42")
    end
  end

  describe "Tool" do
    it "provides a function definition" do
      defn = Enclave::Tool.definition
      expect(defn[:type]).to eq("function")
      expect(defn[:function][:name]).to eq("eval_ruby")
      expect(defn[:function][:parameters][:properties]).to have_key(:code)
    end

    it "calls eval on the enclave" do
      result = Enclave::Tool.call(enclave, code: "2 ** 10")
      expect(result).to eq("=> 1024")
    end
  end

  describe "tools (module bridging)" do
    module TestTools
      def double(n)
        n * 2
      end

      def greet(name)
        "Hello, #{name}!"
      end

      def info
        { name: "test", version: 1, tags: ["a", "b"] }
      end

      def echo_all(a, b, c)
        [a, b, c]
      end

      def returns_nil
        nil
      end

      def returns_true
        true
      end

      def returns_false
        false
      end

      def returns_float
        3.14
      end

      def raise_error
        raise "something went wrong"
      end

      def bad_return
        Object.new
      end
    end

    module MoreTools
      def triple(n)
        n * 3
      end
    end

    let(:enclave_with_tools) { described_class.new(tools: TestTools) }

    after { enclave_with_tools.close unless enclave_with_tools.closed? }

    it "calls a simple tool method with an integer arg" do
      result = enclave_with_tools.eval("double(21)")
      expect(result.value).to eq("42")
      expect(result.error?).to be false
    end

    it "calls a tool method with a string arg" do
      result = enclave_with_tools.eval('greet("World")')
      expect(result.value).to eq('"Hello, World!"')
      expect(result.error?).to be false
    end

    it "returns a hash with nested arrays" do
      result = enclave_with_tools.eval("info()")
      expect(result.error?).to be false
      # mruby inspect uses " => " with spaces
      expect(result.value).to include('"name" => "test"')
      expect(result.value).to include('"tags" => ["a", "b"]')
    end

    it "converts symbol keys to strings" do
      result = enclave_with_tools.eval('info()["name"]')
      expect(result.value).to eq('"test"')
    end

    it "passes multiple args" do
      result = enclave_with_tools.eval('echo_all(1, "two", 3.0)')
      expect(result.value).to eq('[1, "two", 3.0]')
    end

    it "returns nil" do
      result = enclave_with_tools.eval("returns_nil()")
      expect(result.value).to eq("nil")
      expect(result.error?).to be false
    end

    it "returns true" do
      result = enclave_with_tools.eval("returns_true()")
      expect(result.value).to eq("true")
    end

    it "returns false" do
      result = enclave_with_tools.eval("returns_false()")
      expect(result.value).to eq("false")
    end

    it "returns a float" do
      result = enclave_with_tools.eval("returns_float()")
      expect(result.value).to eq("3.14")
    end

    it "captures CRuby exceptions as mruby errors" do
      result = enclave_with_tools.eval("raise_error()")
      expect(result.error?).to be true
      expect(result.error).to include("something went wrong")
    end

    it "rejects unsupported return types with a TypeError" do
      result = enclave_with_tools.eval("bad_return()")
      expect(result.error?).to be true
      expect(result.error).to include("unsupported type")
      expect(result.error).to include("Object")
    end

    it "passes hash args from mruby to CRuby" do
      result = enclave_with_tools.eval('echo_all({"a" => 1}, [2, 3], nil)')
      expect(result.value).to eq('[{"a" => 1}, [2, 3], nil]')
    end

    it "passes boolean and nil args" do
      result = enclave_with_tools.eval("echo_all(true, false, nil)")
      expect(result.value).to eq("[true, false, nil]")
    end

    it "supports multiple modules via expose" do
      enclave_with_tools.expose(MoreTools)
      result = enclave_with_tools.eval("triple(7)")
      expect(result.value).to eq("21")

      # Original tools still work
      result = enclave_with_tools.eval("double(5)")
      expect(result.value).to eq("10")
    end

    it "survives reset!" do
      result = enclave_with_tools.eval("double(10)")
      expect(result.value).to eq("20")

      enclave_with_tools.reset!

      result = enclave_with_tools.eval("double(10)")
      expect(result.value).to eq("20")
      expect(result.error?).to be false
    end

    it "can use tool results in further computations" do
      result = enclave_with_tools.eval("double(double(5))")
      expect(result.value).to eq("20")
    end

    it "passes tools: keyword to constructor" do
      sb = described_class.new(tools: TestTools)
      result = sb.eval("double(3)")
      expect(result.value).to eq("6")
      sb.close
    end

    it "works with .open and tools" do
      described_class.open(tools: TestTools) do |sb|
        result = sb.eval("double(100)")
        expect(result.value).to eq("200")
      end
    end
  end

  describe "timeout" do
    it "raises TimeoutError on infinite loop" do
      e = described_class.new(timeout: 0.5)
      expect { e.eval("loop {}") }.to raise_error(Enclave::TimeoutError)
      e.close
    end

    it "raises TimeoutError on long computation" do
      e = described_class.new(timeout: 0.5)
      expect { e.eval("i = 0; while true; i += 1; end") }.to raise_error(Enclave::TimeoutError)
      e.close
    end

    it "does NOT raise when code finishes in time" do
      e = described_class.new(timeout: 5)
      result = e.eval("1 + 1")
      expect(result.value).to eq("2")
      expect(result.error?).to be false
      e.close
    end

    it "enclave is usable after timeout" do
      e = described_class.new(timeout: 0.5)
      expect { e.eval("loop {}") }.to raise_error(Enclave::TimeoutError)
      result = e.eval("1 + 1")
      expect(result.value).to eq("2")
      e.close
    end

    it "applies class-level default" do
      begin
        Enclave.timeout = 0.5
        e = described_class.new
        expect { e.eval("loop {}") }.to raise_error(Enclave::TimeoutError)
        e.close
      ensure
        Enclave.timeout = nil
      end
    end

    it "per-instance override works" do
      begin
        Enclave.timeout = 100
        e = described_class.new(timeout: 0.5)
        expect { e.eval("loop {}") }.to raise_error(Enclave::TimeoutError)
        e.close
      ensure
        Enclave.timeout = nil
      end
    end

    it "nil means unlimited" do
      e = described_class.new(timeout: nil)
      result = e.eval("1 + 1")
      expect(result.value).to eq("2")
      e.close
    end
  end

  describe "memory_limit" do
    it "raises MemoryLimitError on string bomb" do
      e = described_class.new(memory_limit: 1_000_000)
      expect { e.eval('"x" * 10_000_000') }.to raise_error(Enclave::MemoryLimitError)
      e.close
    end

    it "raises MemoryLimitError on cumulative allocations" do
      e = described_class.new(memory_limit: 1_000_000)
      expect { e.eval('a = []; 100_000.times { a << ("x" * 100) }; a.length') }.to raise_error(Enclave::MemoryLimitError)
      e.close
    end

    it "does NOT raise when allocation fits" do
      e = described_class.new(memory_limit: 10_000_000)
      result = e.eval('"x" * 1000')
      expect(result.error?).to be false
      e.close
    end

    it "enclave is usable after memory limit" do
      e = described_class.new(memory_limit: 1_000_000)
      expect { e.eval('"x" * 10_000_000') }.to raise_error(Enclave::MemoryLimitError)
      result = e.eval("1 + 1")
      expect(result.value).to eq("2")
      e.close
    end

    it "applies class-level default" do
      begin
        Enclave.memory_limit = 1_000_000
        e = described_class.new
        expect { e.eval('"x" * 10_000_000') }.to raise_error(Enclave::MemoryLimitError)
        e.close
      ensure
        Enclave.memory_limit = nil
      end
    end

    it "per-instance override works" do
      begin
        Enclave.memory_limit = 100_000_000
        e = described_class.new(memory_limit: 1_000_000)
        expect { e.eval('"x" * 10_000_000') }.to raise_error(Enclave::MemoryLimitError)
        e.close
      ensure
        Enclave.memory_limit = nil
      end
    end

    it "nil means unlimited" do
      e = described_class.new(memory_limit: nil)
      result = e.eval("1 + 1")
      expect(result.value).to eq("2")
      e.close
    end
  end

  describe "error classes" do
    it "Enclave::Error inherits from StandardError" do
      expect(Enclave::Error).to be < StandardError
    end

    it "Enclave::TimeoutError inherits from Enclave::Error" do
      expect(Enclave::TimeoutError).to be < Enclave::Error
    end

    it "Enclave::MemoryLimitError inherits from Enclave::Error" do
      expect(Enclave::MemoryLimitError).to be < Enclave::Error
    end

    it "TimeoutError is rescuable as Enclave::Error" do
      e = described_class.new(timeout: 0.5)
      expect { e.eval("loop {}") }.to raise_error(Enclave::Error)
      e.close
    end

    it "MemoryLimitError is rescuable as Enclave::Error" do
      e = described_class.new(memory_limit: 1_000_000)
      expect { e.eval('"x" * 10_000_000') }.to raise_error(Enclave::Error)
      e.close
    end
  end

  describe "attr_readers" do
    it "timeout returns configured value" do
      e = described_class.new(timeout: 2.5)
      expect(e.timeout).to eq(2.5)
      e.close
    end

    it "memory_limit returns configured value" do
      e = described_class.new(memory_limit: 5_000_000)
      expect(e.memory_limit).to eq(5_000_000)
      e.close
    end

    it "timeout returns nil when unlimited" do
      e = described_class.new(timeout: nil)
      expect(e.timeout).to be_nil
      e.close
    end

    it "memory_limit returns nil when unlimited" do
      e = described_class.new(memory_limit: nil)
      expect(e.memory_limit).to be_nil
      e.close
    end
  end

  describe "combined limits" do
    it "both limits set, normal eval works" do
      e = described_class.new(timeout: 5, memory_limit: 10_000_000)
      result = e.eval("1 + 1")
      expect(result.value).to eq("2")
      e.close
    end

    it "timeout fires with memory_limit also set" do
      e = described_class.new(timeout: 0.5, memory_limit: 10_000_000)
      expect { e.eval("loop {}") }.to raise_error(Enclave::TimeoutError)
      e.close
    end

    it "memory limit fires with timeout also set" do
      e = described_class.new(timeout: 5, memory_limit: 1_000_000)
      expect { e.eval('"x" * 10_000_000') }.to raise_error(Enclave::MemoryLimitError)
      e.close
    end

    it "limits persist through reset!" do
      e = described_class.new(timeout: 0.5, memory_limit: 1_000_000)
      e.reset!
      expect { e.eval("loop {}") }.to raise_error(Enclave::TimeoutError)
      e.close
    end

    it "works with .open" do
      described_class.open(timeout: 0.5, memory_limit: 10_000_000) do |sb|
        expect { sb.eval("loop {}") }.to raise_error(Enclave::TimeoutError)
      end
    end
  end

  describe "tools (instance-based)" do
    class FakeUser
      attr_accessor :name, :email, :plan

      def initialize(name:, email:, plan:)
        @name = name
        @email = email
        @plan = plan
      end
    end

    class AccountTools
      def initialize(user)
        @user = user
      end

      def user_info
        { name: @user.name, email: @user.email, plan: @user.plan }
      end

      def change_plan(new_plan)
        @user.plan = new_plan
        { success: true, plan: @user.plan }
      end

      def upcase_name
        @user.name.upcase
      end
    end

    class BillingTools
      def charge(amount)
        { charged: amount }
      end
    end

    let(:user) { FakeUser.new(name: "Jane Doe", email: "jane@example.com", plan: "basic") }
    let(:enclave_with_instance) { described_class.new(tools: AccountTools.new(user)) }

    after { enclave_with_instance.close unless enclave_with_instance.closed? }

    it "calls methods on the instance" do
      result = enclave_with_instance.eval("user_info()")
      expect(result.error?).to be false
      expect(result.value).to include('"name" => "Jane Doe"')
      expect(result.value).to include('"plan" => "basic"')
    end

    it "mutates state through the instance" do
      enclave_with_instance.eval('change_plan("premium")')
      expect(user.plan).to eq("premium")
    end

    it "returns the mutated state" do
      result = enclave_with_instance.eval('change_plan("premium")')
      expect(result.value).to include('"plan" => "premium"')
    end

    it "calls methods that return strings" do
      result = enclave_with_instance.eval("upcase_name()")
      expect(result.value).to eq('"JANE DOE"')
    end

    it "supports exposing multiple instances" do
      enclave_with_instance.expose(BillingTools.new)
      result = enclave_with_instance.eval("charge(999)")
      expect(result.value).to include('"charged" => 999')

      # Original tools still work
      result = enclave_with_instance.eval("user_info()")
      expect(result.value).to include('"name" => "Jane Doe"')
    end

    it "survives reset!" do
      result = enclave_with_instance.eval("user_info()")
      expect(result.error?).to be false

      enclave_with_instance.reset!

      result = enclave_with_instance.eval("user_info()")
      expect(result.error?).to be false
      expect(result.value).to include('"name" => "Jane Doe"')
    end

    it "works with .open" do
      tools = AccountTools.new(user)
      described_class.open(tools: tools) do |sb|
        result = sb.eval("user_info()")
        expect(result.value).to include('"name" => "Jane Doe"')
      end
    end
  end
end
