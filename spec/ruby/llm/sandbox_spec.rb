RSpec.describe Ruby::LLM::Sandbox do
  let(:sandbox) { described_class.new }

  after { sandbox.close unless sandbox.closed? }

  describe "#eval" do
    it "evaluates simple expressions" do
      result = sandbox.eval("1 + 1")
      expect(result.value).to eq("2")
      expect(result.error?).to be false
    end

    it "returns inspected value" do
      result = sandbox.eval('"hello"')
      expect(result.value).to eq('"hello"')
    end

    it "returns nil for statements" do
      result = sandbox.eval("x = 42")
      expect(result.error?).to be false
    end

    it "handles multi-line code" do
      code = <<~RUBY
        def add(a, b)
          a + b
        end
        add(2, 3)
      RUBY
      result = sandbox.eval(code)
      expect(result.value).to eq("5")
    end
  end

  describe "state persistence" do
    it "preserves local variables across evals" do
      sandbox.eval("x = 42")
      result = sandbox.eval("x * 2")
      expect(result.value).to eq("84")
    end

    it "preserves method definitions across evals" do
      sandbox.eval("def greet(name); 'Hello ' + name; end")
      result = sandbox.eval("greet('world')")
      expect(result.value).to eq('"Hello world"')
    end

    it "preserves instance variables on top-level self" do
      sandbox.eval("@count = 0")
      sandbox.eval("@count += 1")
      result = sandbox.eval("@count")
      expect(result.value).to eq("1")
    end

    it "stores last result in _" do
      sandbox.eval("42")
      result = sandbox.eval("_ + 8")
      expect(result.value).to eq("50")
    end
  end

  describe "output capture" do
    it "captures puts output" do
      result = sandbox.eval('puts "hello"')
      expect(result.output).to eq("hello\n")
      expect(result.value).to eq("nil")
    end

    it "captures print output" do
      result = sandbox.eval('print "hello"')
      expect(result.output).to eq("hello")
    end

    it "captures p output" do
      result = sandbox.eval('p 42')
      expect(result.output).to eq("42\n")
      expect(result.value).to eq("42")
    end

    it "captures multiple puts" do
      result = sandbox.eval('puts "a"; puts "b"')
      expect(result.output).to eq("a\nb\n")
    end

    it "captures puts with no args" do
      result = sandbox.eval("puts")
      expect(result.output).to eq("\n")
    end

    it "captures puts with arrays" do
      result = sandbox.eval('puts [1, 2, 3]')
      expect(result.output).to eq("1\n2\n3\n")
    end

    it "resets output between evals" do
      sandbox.eval('puts "first"')
      result = sandbox.eval('puts "second"')
      expect(result.output).to eq("second\n")
    end
  end

  describe "error handling" do
    it "captures runtime errors" do
      result = sandbox.eval("1 / 0")
      expect(result.error?).to be true
      expect(result.error).to match(/ZeroDivisionError/)
    end

    it "captures name errors" do
      result = sandbox.eval("undefined_variable_xyz")
      expect(result.error?).to be true
    end

    it "captures syntax errors" do
      result = sandbox.eval("def foo(")
      expect(result.error?).to be true
      expect(result.error).to match(/SyntaxError/)
    end

    it "does not raise Ruby exceptions" do
      expect { sandbox.eval("1 / 0") }.not_to raise_error
    end

    it "allows continued use after errors" do
      sandbox.eval("1 / 0")
      result = sandbox.eval("1 + 1")
      expect(result.value).to eq("2")
      expect(result.error?).to be false
    end
  end

  describe "safety" do
    it "has no File class" do
      result = sandbox.eval("File")
      expect(result.error?).to be true
    end

    it "has no IO class" do
      result = sandbox.eval("IO")
      expect(result.error?).to be true
    end

    it "has no Socket class" do
      result = sandbox.eval("Socket")
      expect(result.error?).to be true
    end

    it "has no Dir class" do
      result = sandbox.eval("Dir")
      expect(result.error?).to be true
    end

    it "has no system() method" do
      result = sandbox.eval('system("echo hi")')
      expect(result.error?).to be true
    end

    it "has no require" do
      result = sandbox.eval('require "json"')
      expect(result.error?).to be true
    end
  end

  describe "#reset!" do
    it "clears local variables" do
      sandbox.eval("x = 42")
      sandbox.reset!
      result = sandbox.eval("x")
      expect(result.error?).to be true
    end

    it "clears method definitions" do
      sandbox.eval("def foo; 1; end")
      sandbox.reset!
      result = sandbox.eval("foo")
      expect(result.error?).to be true
    end

    it "allows continued use after reset" do
      sandbox.reset!
      result = sandbox.eval("1 + 1")
      expect(result.value).to eq("2")
    end
  end

  describe "#close" do
    it "marks sandbox as closed" do
      sandbox.close
      expect(sandbox.closed?).to be true
    end

    it "raises on eval after close" do
      sandbox.close
      expect { sandbox.eval("1") }.to raise_error(RuntimeError, /closed/)
    end

    it "is idempotent" do
      sandbox.close
      expect { sandbox.close }.not_to raise_error
    end
  end

  describe ".open" do
    it "yields a sandbox and auto-closes" do
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
      result = sandbox.eval("1 + 1")
      expect(result).to respond_to(:value)
      expect(result).to respond_to(:output)
      expect(result).to respond_to(:error)
      expect(result).to respond_to(:error?)
    end

    it "has a useful to_s" do
      result = sandbox.eval("1 + 1")
      expect(result.to_s).to eq("=> 2")
    end

    it "includes output in to_s" do
      result = sandbox.eval('puts "hi"; 42')
      expect(result.to_s).to eq("hi\n=> 42")
    end
  end

  describe "Tool" do
    it "provides a function definition" do
      defn = Ruby::LLM::Sandbox::Tool.definition
      expect(defn[:type]).to eq("function")
      expect(defn[:function][:name]).to eq("eval_ruby")
      expect(defn[:function][:parameters][:properties]).to have_key(:code)
    end

    it "calls eval on the sandbox" do
      result = Ruby::LLM::Sandbox::Tool.call(sandbox, code: "2 ** 10")
      expect(result).to eq("=> 1024")
    end
  end
end
