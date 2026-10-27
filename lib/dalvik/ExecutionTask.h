//===- ExecutionTask.h - Retained Dalvik activations -------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include <coroutine>
#include <cstdint>
#include <exception>
#include <utility>

namespace neverd::mobile::dalvik {
// Private symmetric-transfer coroutine. Only the Runtime's external-call
// awaiter yields to its owner; Java calls otherwise remain synchronous.
class ExecutionTask {
public:
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;
  struct promise_type {
    uint64_t value = 0;
    std::exception_ptr error;
    std::coroutine_handle<> caller = std::noop_coroutine();
    ExecutionTask get_return_object() {
      return ExecutionTask(Handle::from_promise(*this));
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct Final {
      bool await_ready() noexcept { return false; }
      std::coroutine_handle<> await_suspend(Handle handle) noexcept {
        return handle.promise().caller;
      }
      void await_resume() noexcept {}
    };
    Final final_suspend() noexcept { return {}; }
    void return_value(uint64_t result) noexcept { value = result; }
    void unhandled_exception() noexcept { error = std::current_exception(); }
  };
  explicit ExecutionTask(Handle handle) : handle(handle) {}
  ExecutionTask(ExecutionTask &&other) noexcept
      : handle(std::exchange(other.handle, {})) {}
  ExecutionTask(const ExecutionTask &) = delete;
  ~ExecutionTask() {
    if (handle)
      handle.destroy();
  }
  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
    handle.promise().caller = caller;
    return handle;
  }
  uint64_t await_resume() {
    if (handle.promise().error)
      std::rethrow_exception(handle.promise().error);
    return handle.promise().value;
  }
  void resume() { handle.resume(); }
  bool done() const { return handle.done(); }

private:
  Handle handle;
};
} // namespace neverd::mobile::dalvik
