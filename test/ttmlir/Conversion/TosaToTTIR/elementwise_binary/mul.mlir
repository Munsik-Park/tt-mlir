// RUN: ttmlir-opt --convert-tosa-to-ttir %s | FileCheck %s
module attributes {} {
  func.func @test_mul(%arg0: tensor<13x21x3xf32>, %arg1: tensor<13x21x3xf32>) -> tensor<13x21x3xf32> {
    %0 = tosa.mul %arg0, %arg1 {shift = 0 : i8} : (tensor<13x21x3xf32>, tensor<13x21x3xf32>) -> tensor<13x21x3xf32>
    // CHECK: [[VAL0:%[0-9]+]] = ttir.empty() : [[TENSOR_SIZE:tensor<13x21x3xf32>]]
    // CHECK: [[VAL1:%[0-9]+]] = "ttir.multiply"(%arg{{[0-9]+}}, %arg{{[0-9]+}}, [[VAL0]]) : ([[TENSOR_SIZE]], [[TENSOR_SIZE]], [[TENSOR_SIZE]]) -> [[TENSOR_SIZE]]
    return %0 : tensor<13x21x3xf32>
    // CHECK: return [[VAL1]] : [[TENSOR_SIZE]]
  }
}
