// RUN: ttmlir-opt --convert-ttir-to-linalg %s | FileCheck %s

module {
  func.func @test_exp(%arg0: tensor<13x21x3xf32>) -> tensor<13x21x3xf32> {
    // CHECK: [[VAL0:%[0-9]+]] = tensor.empty() : [[SIZE:tensor<13x21x3xf32>]]
    %0 = ttir.empty() : tensor<13x21x3xf32>
    // CHECK: [[VAL1:%[0-9]+]] = linalg.exp ins(%arg0 : [[SIZE]]) outs([[VAL0]] : [[SIZE]]) -> [[SIZE]]
    %1 = "ttir.exp"(%arg0, %0) : (tensor<13x21x3xf32>, tensor<13x21x3xf32>) -> tensor<13x21x3xf32>
    // CHECK: return [[VAL1]] : [[SIZE]]
    return %1 : tensor<13x21x3xf32>
  }
}
