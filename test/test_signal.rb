# frozen_string_literal: true

require_relative 'helper'

class SignalTrapTest < Minitest::Test
  def test_signal_exception_handling
    i, o = IO.pipe
    pid = Polyphony.fork do
      i.close
      spin do
        spin do
          sleep 5
        rescue ::Interrupt => e
          # the signal should be raised only in the main fiber
          o.puts "1-interrupt"
        end.await
      end.await
    rescue ::Interrupt => e
      o.puts "3-interrupt"
    ensure
      o.close
    end
    sleep 0.01
    o.close
    Process.kill('INT', pid)
    Thread.current.backend.waitpid(pid)
    buffer = i.read
    assert_equal "3-interrupt\n", buffer
  end

  def test_signal_exception_with_cleanup
    i, o = IO.pipe
    pid = Polyphony.fork do
      i.close
      spin do
        spin do
          sleep
        rescue Polyphony::Terminate
          o.puts "1 - terminated"
        end.await
      rescue Polyphony::Terminate
        o.puts "2 - terminated"
      end.await
    rescue Interrupt
      o.puts "3 - interrupted"
      Fiber.current.terminate_all_children
      Fiber.current.await_all_children
    ensure
      o.close
    end
    sleep 0.02
    o.close
    Process.kill('INT', pid)
    Thread.current.backend.waitpid(pid)
    buffer = i.read
    assert_equal "3 - interrupted\n2 - terminated\n1 - terminated\n", buffer
  end

  def test_interrupt_signal_scheduling
    i, o = IO.pipe
    pid = Polyphony.fork do
      i.close
      sleep
    rescue ::Interrupt => e
      o.puts '3-interrupt'
    ensure
      o.close
    end
    o.close
    sleep 0.1
    Process.kill('INT', pid)
    Thread.current.backend.waitpid(pid)
    buffer = i.read
    assert_equal "3-interrupt\n", buffer
  end
end