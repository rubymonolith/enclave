require "bundler/inline"

gemfile do
  source "https://rubygems.org"
  gem "enclave", path: ".."
  gem "ruby_llm"
  gem "activerecord", require: "active_record"
  gem "sqlite3"
end

ENV["ANTHROPIC_API_KEY"] ||= ENV["ENCLAVE_ANTHROPIC_API_KEY"] || begin
  print "Anthropic API key: "
  gets.strip
end

# ── Database setup ──────────────────────────────────────────────────────────────

ActiveRecord::Base.establish_connection(adapter: "sqlite3", database: ":memory:")
ActiveRecord::Base.logger = nil

ActiveRecord::Schema.define do
  create_table :customers do |t|
    t.string :name, null: false
    t.string :email, null: false
    t.string :plan, default: "basic"
    t.timestamps
  end

  create_table :orders do |t|
    t.belongs_to :customer, null: false
    t.decimal :total, precision: 10, scale: 2
    t.string :status, default: "pending"
    t.timestamps
  end

  create_table :support_tickets do |t|
    t.belongs_to :customer, null: false
    t.string :subject, null: false
    t.text :body
    t.string :status, default: "open"
    t.timestamps
  end
end

# ── Models ──────────────────────────────────────────────────────────────────────

class Customer < ActiveRecord::Base
  has_many :orders
  has_many :support_tickets
end

class Order < ActiveRecord::Base
  belongs_to :customer
end

class SupportTicket < ActiveRecord::Base
  belongs_to :customer
end

# ── Seed data from DATA ─────────────────────────────────────────────────────────

require "yaml"
seed = YAML.safe_load(DATA.read)

seed["customers"].each do |c|
  customer = Customer.create!(name: c["name"], email: c["email"], plan: c["plan"])
  c["orders"]&.each { |o| customer.orders.create!(total: o["total"], status: o["status"]) }
  c["tickets"]&.each { |t| customer.support_tickets.create!(subject: t["subject"], body: t["body"], status: t["status"]) }
end

# ── Tool class (scoped to one customer) ────────────────────────────────────────

class CustomerServiceTools
  def initialize(customer)
    @customer = customer
  end

  def customer_info
    { id: @customer.id, name: @customer.name, email: @customer.email,
      plan: @customer.plan, created_at: @customer.created_at.to_s }
  end

  def orders
    @customer.orders.order(created_at: :desc).map do |o|
      { id: o.id, total: o.total.to_f, status: o.status, created_at: o.created_at.to_s }
    end
  end

  def update_email(new_email)
    @customer.update!(email: new_email)
    { success: true, email: @customer.reload.email }
  end

  def list_tickets
    @customer.support_tickets.order(created_at: :desc).map do |t|
      { id: t.id, subject: t.subject, body: t.body, status: t.status, created_at: t.created_at.to_s }
    end
  end

  def get_ticket(ticket_id)
    t = @customer.support_tickets.find(ticket_id)
    { id: t.id, subject: t.subject, body: t.body, status: t.status, created_at: t.created_at.to_s }
  end

  def create_ticket(subject, body)
    t = @customer.support_tickets.create!(subject: subject, body: body)
    get_ticket(t.id)
  end

  def update_ticket(ticket_id, fields)
    ticket = @customer.support_tickets.find(ticket_id)
    allowed = fields.slice("subject", "body", "status")
    ticket.update!(allowed)
    get_ticket(ticket_id)
  end
end

# ── RubyLLM tool wrapping the sandbox ──────────────────────────────────────────

class CustomerServiceConsole < RubyLLM::Tool
  description "Run Ruby code in a sandboxed customer service console. " \
              "The code has access to functions for looking up and modifying " \
              "the current customer's data. Returns the result of the evaluation."

  param :code, desc: "Ruby code to evaluate"

  def execute(code:)
    puts "\n\e[2m  enclave> #{code.gsub("\n", "\n  enclave> ")}"
    result = Enclave::Tool.call(@@enclave, code: code)
    puts "       => #{result}\e[0m"
    result.force_encoding("UTF-8")
  end

  def self.connect(enclave)
    @@enclave = enclave
  end
end

# ── Wire it all up ──────────────────────────────────────────────────────────────

customer = Customer.find(1)
enclave = Enclave.new(tools: CustomerServiceTools.new(customer))
CustomerServiceConsole.connect(enclave)

RubyLLM.configure do |config|
  config.anthropic_api_key = ENV["ANTHROPIC_API_KEY"]
  config.default_model = "claude-sonnet-4-20250514"
end

chat = RubyLLM::Chat.new(model: "claude-sonnet-4-20250514")
chat.with_tool(CustomerServiceConsole)

chat.with_instructions <<~PROMPT
  You are a customer service agent. You have access to a Ruby console where you
  can run code to look up and modify the current customer's data.

  Available functions in the console:

    customer_info          - Returns a hash with the customer's id, name, email, plan, created_at
    orders                 - Returns an array of the customer's orders (id, total, status, created_at)
    update_email(email)    - Updates the customer's email address
    list_tickets              - Returns all support tickets (id, subject, body, status, created_at)
    get_ticket(id)            - Returns a single ticket by id
    create_ticket(subject, body) - Creates a new support ticket
    update_ticket(id, fields) - Updates a ticket's subject, body, or status. No deleting.

  There is no delete function. Tickets cannot be deleted, only updated.

  You can also use normal Ruby to filter, sort, or compute over the results.
  Always look up the data before answering — don't guess.
PROMPT

# ── Interactive chat loop ───────────────────────────────────────────────────────

puts
puts "Customer Service Agent (serving: #{customer.name})"
puts "Type 'help' to see what you can ask, or 'exit' to quit."
puts

HELP = <<~HELP
  Try asking things like:
    - What's this customer's email?
    - Show me their orders
    - Do they have any open tickets?
    - Show me the details of ticket #1
    - Close ticket #1
    - Create a ticket about needing a refund
    - Update ticket #1's subject to "Resolved: order status"
    - Try to delete a ticket (you can't!)
HELP

loop do
  print "🧑: "
  input = gets&.strip
  break if input.nil? || input.downcase == "exit"
  next if input.empty?
  if input.downcase == "help"
    puts HELP
    next
  end

  response = chat.ask(input)
  puts "\n🤖: #{response.content}\n\n"
end

enclave.close
puts "Goodbye!"

__END__
customers:
  - name: Alice Johnson
    email: alice@example.com
    plan: premium
    orders:
      - total: 99.99
        status: shipped
      - total: 149.50
        status: delivered
      - total: 29.00
        status: pending
    tickets:
      - subject: Where is my order?
        body: "Order #3 still shows pending."
        status: open
      - subject: Billing question
        body: Was I charged twice?
        status: closed

  - name: Bob Smith
    email: bob@example.com
    plan: basic
    orders:
      - total: 59.99
        status: delivered
    tickets:
      - subject: Can't log in
        body: Password reset not working.
        status: open

  - name: Carol Davis
    email: carol@example.com
    plan: enterprise
    orders:
      - total: 499.00
        status: shipped
      - total: 250.00
        status: pending
    tickets:
      - subject: Feature request
        body: Please add dark mode.
        status: open
