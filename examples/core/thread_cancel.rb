# frozen_string_literal: true

require 'modulation'
require 'digest'
require 'socket'

Rubato     = import('../../lib/rubato')

@op_count = 0

def lengthy_op
  @op_count += 1
  acc = 0
  count = 0
  100.times { acc += IO.read('../../docs/reality-ui.bmpr').bytesize; count += 1; p count }
  acc / count
end

spawn do
  t0 = Time.now
  cancel_after(0.01) do
    data = await Rubato::Thread.spawn { lengthy_op }
    puts "read #{data.bytesize} bytes (#{Time.now - t0}s)"
  end
rescue Exception => e
  puts "error: #{e}"
ensure
  p @op_count
end
