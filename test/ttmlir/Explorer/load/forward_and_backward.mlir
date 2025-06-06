// RUN: ttmlir-opt --ttir-to-ttnn-backend-pipeline %s
// Need to ensure that model is valid MLIR module

module @SimpleModel attributes {} {
  func.func @forward(%arg0: tensor<1x784xf32> {ttir.name = "input_1"}, %arg1: tensor<10x784xf32> {ttir.name = "linear.weight"}, %arg2: tensor<10xf32> {ttir.name = "linear.bias"}) -> (tensor<1x10xf32> {ttir.name = "SimpleModel_472.output_softmax_1495"}) {
    %0 = ttir.empty() : tensor<784x10xf32>
    %1 = "ttir.transpose"(%arg1, %0) <{dim0 = -2 : si32, dim1 = -1 : si32}> : (tensor<10x784xf32>, tensor<784x10xf32>) -> tensor<784x10xf32>
    %2 = ttir.empty() : tensor<1x10xf32>
    %3 = "ttir.matmul"(%arg0, %1, %2) : (tensor<1x784xf32>, tensor<784x10xf32>, tensor<1x10xf32>) -> tensor<1x10xf32>
    %4 = ttir.empty() : tensor<1x10xf32>
    %5 = "ttir.add"(%3, %arg2, %4) : (tensor<1x10xf32>, tensor<10xf32>, tensor<1x10xf32>) -> tensor<1x10xf32>
    %6 = ttir.empty() : tensor<1x10xf32>
    %7 = "ttir.softmax"(%5, %6) <{dimension = -1 : si32}> : (tensor<1x10xf32>, tensor<1x10xf32>) -> tensor<1x10xf32>
    return %7 : tensor<1x10xf32>
  }
  func.func @backward(%arg0: tensor<1x10xf32> {ttir.name = "loss_SimpleModel_472.output_softmax_1495"}, %arg1: tensor<1x10xf32> {ttir.name = "SimpleModel_472.output_softmax_1495"}, %arg2: tensor<1x784xf32> {ttir.name = "input_1"}) -> (tensor<1x10xf32> {ttir.name = "grad_acc_linear.bias_grad_accumulator"}, tensor<10x784xf32> {ttir.name = "grad_acc_linear.weight_grad_accumulator"}) {
    %0 = ttir.empty() : tensor<1x10xf32>
    %1 = "ttir.multiply"(%arg0, %arg1, %0) : (tensor<1x10xf32>, tensor<1x10xf32>, tensor<1x10xf32>) -> tensor<1x10xf32>
    %2 = ttir.empty() : tensor<1x1xf32>
    %3 = "ttir.sum"(%1, %2) <{keep_dim = true}> : (tensor<1x10xf32>, tensor<1x1xf32>) -> tensor<1x1xf32>
    %4 = ttir.empty() : tensor<1x10xf32>
    %5 = "ttir.subtract"(%arg0, %3, %4) : (tensor<1x10xf32>, tensor<1x1xf32>, tensor<1x10xf32>) -> tensor<1x10xf32>
    %6 = ttir.empty() : tensor<1x10xf32>
    %7 = "ttir.multiply"(%5, %arg1, %6) : (tensor<1x10xf32>, tensor<1x10xf32>, tensor<1x10xf32>) -> tensor<1x10xf32>
    %8 = ttir.empty() : tensor<784x1xf32>
    %9 = "ttir.transpose"(%arg2, %8) <{dim0 = -2 : si32, dim1 = -1 : si32}> : (tensor<1x784xf32>, tensor<784x1xf32>) -> tensor<784x1xf32>
    %10 = ttir.empty() : tensor<784x10xf32>
    %11 = "ttir.matmul"(%9, %7, %10) : (tensor<784x1xf32>, tensor<1x10xf32>, tensor<784x10xf32>) -> tensor<784x10xf32>
    %12 = ttir.empty() : tensor<10x784xf32>
    %13 = "ttir.transpose"(%11, %12) <{dim0 = -2 : si32, dim1 = -1 : si32}> : (tensor<784x10xf32>, tensor<10x784xf32>) -> tensor<10x784xf32>
    return %7, %13 : tensor<1x10xf32>, tensor<10x784xf32>
  }
}
