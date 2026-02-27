# Ruby LLM Sandbox

## Why this exists

You're adding an AI agent to your Rails app. The agent needs to look up orders, update tickets, maybe change a customer's email. The standard approach is tool calling — you define discrete functions, the LLM picks which one to call, you execute it.

That works. But it's limiting. If a customer asks "what's my total spend on shipped orders this year?", you either need a `total_spend_by_status_and_date_range` tool (which you didn't build) or the agent has to make multiple round-trips: fetch all orders, then… well, it can't do math. You need another tool for that. The tool list grows, each one is an LLM round-trip, and you're forever playing catch-up with the questions your users actually ask.

The alternative is to let the agent write code. One `eval` tool replaces dozens of specialized tools. The agent fetches orders and filters them in a single call:

```ruby
orders().select { |o| o["status"] == "shipped" }.sum { |o| o["total"] }
```

The problem is obvious: `eval` in your Ruby process is catastrophic. The agent can do anything your app can do — `User.destroy_all`, `File.read("/etc/passwd")`, `ENV["SECRET_KEY_BASE"]`, `system("curl attacker.com")`. One prompt injection in a ticket body and you're done.

This gem gives you `eval` without the blast radius. It embeds a separate MRuby VM — an isolated Ruby interpreter with no file system, no network, no access to your CRuby runtime. You expose specific functions into it. The agent writes code against those functions and nothing else.

```ruby
class CustomerServiceTools
  def initialize(user)
    @user = user
  end

  def user_info
    { name: @user.name, email: @user.email, plan: @user.plan,
      created_at: @user.created_at.to_s }
  end

  def change_plan(new_plan)
    @user.update!(plan: new_plan)
    { success: true, plan: @user.reload.plan }
  end

  def recent_tickets
    @user.support_tickets.order(created_at: :desc).limit(10).map do |t|
      { id: t.id, subject: t.subject, status: t.status }
    end
  end
end

user = User.find(params[:user_id])
sandbox = Ruby::LLM::Sandbox.new(tools: CustomerServiceTools.new(user))
```

Inside the sandbox, the agent sees these functions and nothing else:

```ruby
user_info()
#=> {"name" => "Jane Doe", "email" => "jane@example.com", "plan" => "basic", ...}

change_plan("premium")
#=> {"success" => true, "plan" => "premium"}

open_tickets = recent_tickets().select { |t| t["status"] == "open" }
open_tickets.length
#=> 3
```

There's no `User` class in the sandbox. No ActiveRecord. No file system. No network. The agent can only call the methods you gave it, scoped to the user you passed in.

### Do you actually need this?

If your agent only needs to pick from a fixed menu of actions — "cancel order", "send refund", "update email" — standard tool calling is fine. Each tool is a function the LLM selects; you control the surface area; there's no code execution to worry about.

The sandbox becomes worth it when:

- **The agent needs to reason over data.** Filter, sort, aggregate, compare. Instead of building a tool for every possible query, you expose the raw data and let the agent write the logic.
- **You want fewer round-trips.** One sandbox eval can fetch data, process it, and return a result. That's one LLM turn instead of three or four.
- **You can't predict the questions.** Customer service, data exploration, internal dashboards — anywhere users ask ad-hoc questions about their own data.

## Installation

Add to your Gemfile:

```ruby
gem "ruby-llm-sandbox"
```

The gem builds MRuby from source on first compile, so the initial `bundle install` takes a moment.

## Quick start

There's a complete working example in [`examples/rails.rb`](examples/rails.rb) — a single-file app with SQLite, ActiveRecord, and an interactive chat loop. Run it with:

```bash
ruby examples/rails.rb
```

## Defining tools

Write a class. Initialize it with whatever data the agent should have access to. Its public methods become the functions the agent can call.

```ruby
class OrderTools
  def initialize(order)
    @order = order
  end

  def details
    { id: @order.id, total: @order.total.to_f, status: @order.status,
      items: @order.line_items.map { |li| { name: li.name, qty: li.qty } } }
  end

  def apply_discount(percent)
    raise "discount must be 1-50%" unless (1..50).cover?(percent)
    @order.apply_discount!(percent)
    { success: true, new_total: @order.reload.total.to_f }
  end

  def cancel
    @order.cancel!
    { success: true }
  end
end

sandbox = Ruby::LLM::Sandbox.new(tools: OrderTools.new(order))
```

### Multiple tool objects

```ruby
sandbox = Ruby::LLM::Sandbox.new(tools: AccountTools.new(user))
sandbox.expose(BillingTools.new(user.billing_account))
sandbox.expose(NotificationTools.new(user))
```

All methods from all exposed objects are available as functions in the sandbox.

### Allowed types

Values crossing the boundary must be one of:

| Type | Notes |
|------|-------|
| `nil`, `true`, `false` | |
| `Integer`, `Float` | |
| `String` | |
| `Symbol` | Converted to `String` automatically |
| `Array` | Elements must be allowed types |
| `Hash` | Keys and values must be allowed types |

If a method returns something else, the agent gets a clear error:

```
TypeError: unsupported type for sandbox: User
```

This means you need to serialize your data into hashes — which is a feature, not a bug. It forces you to be explicit about what the agent can see.

### Error handling

Exceptions in your tool methods are caught and returned as errors. The sandbox keeps running:

```ruby
# Inside the sandbox:
apply_discount(99)   #=> RuntimeError: discount must be 1-50%
details()            # still works
```

## Safety

If you run agent-generated code with `eval` in CRuby, the agent can do anything your app can do. Here's what happens when you try those same things inside the sandbox:

```ruby
sandbox.eval('File.read("/etc/passwd")')
#=> NameError: uninitialized constant File

sandbox.eval('ENV["SECRET_KEY_BASE"]')
#=> NameError: uninitialized constant ENV

sandbox.eval('`curl http://attacker.com`')
#=> NotImplementedError: backquotes not implemented
```

These aren't runtime permission checks — the classes and methods simply don't exist. MRuby is a separate interpreter compiled without IO, network, or process modules. There's nothing to bypass.

Each sandbox instance is fully isolated from other instances.

## License

MIT
