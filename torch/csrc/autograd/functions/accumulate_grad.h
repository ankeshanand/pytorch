#pragma once

#include <torch/csrc/autograd/function.h>
#include <torch/csrc/autograd/variable.h>
#include <torch/csrc/WindowsTorchApiMacro.h>

namespace torch { namespace autograd {

struct TORCH_API AccumulateGrad : public Node {
  explicit AccumulateGrad(Variable variable_);

  variable_list apply(variable_list&& grads) override;

  // Given a variable with its current grad as variable_grad, accumulates
  // new_grad into variable_grad if in place accumulation is possible.
  // Otherwise, uses 'update_grad' to update the grad for the variable.

  // variable: the variable whose grad we're accumulating.
  // variable_grad: the current grad for the variable.
  // new_grad: new grad we want to acummulate for the variable.
  // num_expected_refs: the number of refs we expect to hold internally
  //                    such that it is safe to avoid cloning the grad
  //                    if use_count() of the grad is less than or equal
  //                    to this value (in addition to post_hooks).
  // update_grad: Function that is used to update grad for the variable.
  //              The argument to the function is a Tensor which
  //              is used to set a new value for the grad.
  template <typename T>
  static void accumulateGradAndCallHooks(
      const Variable& variable,
      at::Tensor variable_grad,
      const at::Tensor& new_grad,
      size_t num_expected_refs,
      const T& update_grad) {
    // Copy since we need to work with non-const Tensor. Grab the original
    // use_count beforehand though.
    size_t new_grad_use_count = new_grad.use_count();
    at::Tensor new_grad_copy = new_grad;

    for (auto& hook : impl::hooks(variable)) {
      new_grad_copy = (*hook)({new_grad_copy})[0];
    }

    if (!variable_grad.defined()) {
      // under following condition, we can avoid clone()
      if (!GradMode::is_enabled() && !new_grad_copy.is_sparse() &&
          new_grad_copy.is_contiguous() &&
          new_grad_use_count <= num_expected_refs) {
        // first check it is in first-order grad only mode
        // then check not sparse before is_contiguous
        // then check contiguous, otherwise later in place accumulation may fail
        // and lastly, check if the use_count is less than or equal to the
        // number of references we expect before grabbing it. The number of
        // references we expect is basically internal structures that are
        // holding references to the Tensor and that is fine since these are not
        // exposed to the user.
        update_grad(new_grad_copy.detach());
      } else if (
          !GradMode::is_enabled() && new_grad_copy.is_sparse() &&
          new_grad_copy._indices().is_contiguous() &&
          new_grad_copy._values().is_contiguous() &&
          new_grad_use_count <= num_expected_refs) {
        // Can't detach sparse tensor (since metadata changes are not allowed
        // after detach), so just create a new one for the grad which is a
        // shallow copy. We need a shallow copy so that modifying the original
        // grad tensor doesn't modify the grad we accumulate.
        // We only skip clone if indices and values themselves are contiguous
        // for backward compatiblity reasons. Since without this optimization,
        // earlier we would clone the entire SparseTensor which cloned indices
        // and values.
        update_grad(at::sparse_coo_tensor(
            new_grad_copy._indices(),
            new_grad_copy._values(),
            new_grad_copy.sizes(),
            new_grad_copy.options()));
      } else {
        if (new_grad_copy.is_sparse()) {
          update_grad(new_grad_copy.clone());
        } else {
          update_grad(new_grad_copy.clone(at::MemoryFormat::Contiguous));
        }
      }
    } else if (!GradMode::is_enabled()) {
      // This case is not strictly necessary, but it makes the first-order only
      // case slightly more efficient.
      if (variable_grad.is_sparse() && !new_grad_copy.is_sparse()) {
        // If `variable_grad` is sparse and `new_grad_copy` is not sparse, their
        // sum is not sparse, and we must change the TensorImpl type of
        // `variable_grad` for it to store the result. However, changing the
        // TensorImpl type of a tensor requires changing the tensor itself, and
        // thus in this case we have to change the grad tensor.
        update_grad(new_grad_copy + variable_grad);
      } else {
        // In this case we can avoid changing the grad tensor. There are three
        // scenarios when we'll hit this case:
        //
        // 1. `variable_grad` is sparse, and `new_grad_copy` is sparse.
        // 2. `variable_grad` is dense, and `new_grad_copy` is sparse.
        // 3. `variable_grad` is dense, and `new_grad_copy` is dense.
        //
        // In all of these three cases, `variable_grad += new_grad_copy` is a
        // valid operation which adds `new_grad_copy` to `variable_grad` in
        // place. `variable_grad` is thus still referring to the same tensor
        // after the operation.
        variable_grad += new_grad_copy;
      }
    } else {
      update_grad(variable_grad + new_grad_copy);
    }
  }

  Variable variable;
};

} // namespace autograd
} // namespace torch
