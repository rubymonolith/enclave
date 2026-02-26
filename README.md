# Ruby LLM Sandbox

Give an AI agent scoped access to your Rails app without risking data leakage. You write a plain Ruby class. The agent can call its methods — and nothing else.

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

## Installation

Add to your Gemfile:

```ruby
gem "ruby-llm-sandbox"
```

The gem builds MRuby from source on first compile, so the initial `bundle install` takes a moment.

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

The MRuby sandbox has no access to:

- File I/O (`File`, `IO`, `Dir`)
- Network (`Socket`, `Net::HTTP`)
- System commands (`system`, backticks, `exec`)
- `require` / `load`
- The host CRuby runtime

Each sandbox instance is fully isolated from other instances.

## License

MIT
