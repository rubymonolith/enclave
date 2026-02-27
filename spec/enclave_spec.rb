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
    it "has no File class" do
      result = enclave.eval("File")
      expect(result.error?).to be true
    end

    it "has no IO class" do
      result = enclave.eval("IO")
      expect(result.error?).to be true
    end

    it "has no Socket class" do
      result = enclave.eval("Socket")
      expect(result.error?).to be true
    end

    it "has no Dir class" do
      result = enclave.eval("Dir")
      expect(result.error?).to be true
    end

    it "has no system() method" do
      result = enclave.eval('system("echo hi")')
      expect(result.error?).to be true
    end

    it "has no require" do
      result = enclave.eval('require "json"')
      expect(result.error?).to be true
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
