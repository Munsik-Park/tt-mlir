// REQUIRES: stablehlo
// RUN: ttmlir-opt --stablehlo-to-ttir-pipeline %s | FileCheck %s
module @jit_eltwise_neg attributes {} {
  func.func public @test_neg(%arg0: tensor<13x21x3xf32>) -> tensor<13x21x3xf32> {
    %0 = stablehlo.negate %arg0 : tensor<13x21x3xf32>
    // CHECK: = ttir.empty
    // CHECK: = "ttir.neg"
    return %0 : tensor<13x21x3xf32>
  }
}
