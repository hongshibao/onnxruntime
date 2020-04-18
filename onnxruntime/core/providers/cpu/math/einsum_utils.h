// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// This module hosts 3 abstractions -

// 1) EinsumEquationPreprocessor - 
// Holds logic to statically pre-process the equation string (i.e.) without input shapes being known
// These need not be repeated at Compute() time again

// 2) EinsumComputePreprocessor - 
// Holds logic to process the data from  EinsumEquationPreprocessor using known input shapes to parse data required 
// during Einsum Compute(). For example, mapping subscript labels to a dimension value, etc.

// 3) EinsumTypedComputeProcessor - The core logic of the Einsum operator. Invoked from Einsum Compute().

#pragma once

#include "einsum_auxiliary_ops.h"

namespace onnxruntime {

namespace EinsumOp {

constexpr size_t num_of_letters = 26;

}  // namespace EinsumOp

struct EinsumEquationPreprocessor {
  explicit EinsumEquationPreprocessor(const std::string& einsum_equation) {
    // Make copy of the equation as it will be mutated
    einsum_preprocessed_equation_ = einsum_equation;

    // Remove space characters in the copy of the Einsum eqution
    einsum_preprocessed_equation_.erase(std::remove(einsum_preprocessed_equation_.begin(), einsum_preprocessed_equation_.end(), ' '),
                                        einsum_preprocessed_equation_.end());

    // Check if the Einsum equation has the output subscript labels
    auto mid_index = einsum_preprocessed_equation_.find("->");
    if (mid_index != std::string::npos) {
      // Separate right and left hand sides of the equation
      left_equation_ = einsum_preprocessed_equation_.substr(0, mid_index);
      right_equation_ = einsum_preprocessed_equation_.substr(mid_index + 2);
      is_explicit_ = true;
    } else {
      left_equation_ = einsum_preprocessed_equation_;
    };

    // Process the left_equation_ by splitting on ','
    std::string delimiter = ",";
    size_t pos = 0;
    std::string token;
    while ((pos = left_equation_.find(delimiter)) != std::string::npos) {
      token = left_equation_.substr(0, pos);
      left_equation_.erase(0, pos + delimiter.length());
      left_equation_split_.push_back(token);  // This copy is done statically at model load, hence should not affect runtime perf
    }
    left_equation_split_.push_back(left_equation_);  // This holds the portion of the equation after the last ','
  }

  // Holds the pre-processed equation string
  // In theory, we could re-write the einsum equation to lower overall cost of intermediate arrays
  // See numpy.einsum_path for details/examples
  // These are very advanced optimizations that we don't require for the average use-case
  std::string einsum_preprocessed_equation_;

  // In explicit form, holds the left side of the einsum equation
  // (e.g.) Einsum equation = 'i,j->i', then left_equation_ = 'i,j'
  // In implicit form, holds the entire einsum equation
  // (e.g.) Einsum equation = 'i,j', then left_equation_ = 'i,j'
  std::string left_equation_;

  // Holds the strings obtained after splitting left_equation_ on ','
  std::vector<std::string> left_equation_split_;

  // Holds constructed or parsed output subscript
  std::string right_equation_;

  // Flag indicating if the Einsum op is being used in explicit form (i.e.) contains '->'
  bool is_explicit_ = false;
};

// Prologue:
// In the sample Einsum string: 'ij, jk'
// Subscripts are 'ij' and 'jk'
// Subscript labels are 'i', 'j', and 'k'
// Subscript labels (letter) and subcript indices (a unique id to the letter) are interchangeable

// This is a pre-processor class that maps subscript labels to a dimension value, etc.
class EinsumComputePreprocessor final {
 public:
  explicit EinsumComputePreprocessor(EinsumEquationPreprocessor& equation_preprocessor,
                                     const std::vector<const Tensor*>& inputs,
                                     AllocatorPtr allocator);

  // Get the output dims of the op's output
  const std::vector<int64_t>& GetOutputDims() const;

  // Process all input tensors so that they are all of the same rank and easy to perfrom MatMul and reduction on
  std::vector<Tensor>& GetPreprocessedTensors();

  // For each subscript index, hold the last input the subscript index was seen in
  const std::vector<int64_t>& GetMappedSubscriptIndicesToLastInputIndex() const;

  // For each subscript index, hold the index it corresponds to in the output's shape
  const std::vector<int64_t>& GetMappedSubscriptIndicesToOutputindices() const;

  // Get the number of subscript indices (subscript labels) in the einsum equation
  int64_t GetNumSubscriptIndices() const;

 private:
  // Process subscripts of each input and collect metadata along the way
  void ProcessSubscripts();

  // A function to process bradcasted dims (ellipsis) of inputs that they occur in
  void PostProcessBroadcastedDims();

  // Check if the Einsum equation has an explicit form (equation string contains "->")
  // If it is of explicit form, parse the output subscript (substring following "->")
  // If it is of implicit form (equation string does not contain "->"), compose the output subscript
  // If the output subscript is an empty string, the result is a scalar
  void ParseOrCreateOutputSubscript();

  void CalculateOutputShape();

  void PreprocessInputs();

  // private members
  // Instance of EinsumEquationPreprocessor
  EinsumEquationPreprocessor einsum_equation_preprocessor_;

  // The number of dims that encompasses an "ellipsis"
  size_t num_of_ellipsis_dims_ = 0;

  // All original inputs to the op
  const std::vector<const Tensor*>& inputs_;

  // All preprocessed inputs
  std::vector<Tensor> preprocessed_inputs_;

  // Count of unique subscript labels (subscript indices)
  // E.g. 1 : With equation -> 'ij, jk -> ik'
  // num_subscript_indices_ = 3 (i, j, k)
  // E.g. 2 : With equation -> '...ij', 'jk' -> '...ik'
  // num_subscript_indices_ = 3 (i, j, k) + number of dims specified by an ellipsis (across all inputs)
  int64_t num_subscript_indices_ = 0;

  // Hold the count corresponding to the letter seen
  // `0` means the corresponding letter wasn't seen at all
  std::array<int64_t, EinsumOp::num_of_letters> letter_to_count_;

  // Hold the assigned index corresponding to the letter seen
  // `-1` means the corresponding letter wasn't seen at all
  std::array<int64_t, EinsumOp::num_of_letters> letter_to_index_;

  // Holds the input index of the last input to have the index correpesponding to the subscript label
  // If the value is `-1`, then the subscript label is never seen (or) it appears in the output
  std::vector<int64_t> subscript_indices_to_last_input_;

  // Hold the dim value of the index correpesponding to the subscript label
  // `-1` means the corresponding label wasn't seen at all
  std::vector<int64_t> subscript_indices_to_dim_value_;

  // Holds the final calculated output dimensions
  std::vector<int64_t> output_dims_;

  // All subscript indices in the equation for each input
  std::vector<std::vector<int64_t>> input_subscript_indices_;

  // Index corresponding to each output dim corresponding to each subscript index
  // A value of -1 means the corresponding subscript index is not founf in the output
  std::vector<int64_t> subscript_indices_to_output_indices_;

  // Allocator to use for ad-hoc tensor buffer allocation
  AllocatorPtr allocator_;
};

// This method does the heavy-lifting compute portion of Einsum Compute()
template <typename T>
Status EinsumTypedComputeProcessor(OpKernelContext* context, AllocatorPtr allocator, 
                                   EinsumComputePreprocessor& einsum_compute_preprocessor);

}  // namespace onnxruntime