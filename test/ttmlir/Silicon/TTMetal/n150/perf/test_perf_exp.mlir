// UNSUPPORTED: true
// RUN: ttmlir-opt --ttir-to-ttmetal-backend-pipeline="system-desc-path=%system_desc_path%" %s > %t.mlir
// RUN: FileCheck %s --input-file=%t.mlir
// RUN: ttmlir-translate --ttmetal-to-flatbuffer %t.mlir > %t.ttm

func.func @exp(%arg0: tensor<64x128xf32>) -> tensor<64x128xf32> {
  // CHECK: = "ttmetal.create_buffer"
  %0 = ttir.empty() : tensor<64x128xf32>
  // CHECK: = "ttmetal.enqueue_program"
  %1 = "ttir.exp"(%arg0, %0) : (tensor<64x128xf32>, tensor<64x128xf32>) -> tensor<64x128xf32>
  return %1 : tensor<64x128xf32>
}
