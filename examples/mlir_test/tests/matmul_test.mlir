func.func public @matmul(%A: tensor<4x8xf64>, %B: tensor<8x4xf64>, %C: tensor<4x4xf64>) -> tensor<4x4xf64> {
  %C.out = linalg.generic {indexing_maps = [affine_map<(d0, d1, d2) -> (d0, d2)>, affine_map<(d0, d1, d2) -> (d2, d1)>, affine_map<(d0, d1, d2) -> (d0, d1)>], iterator_types = ["parallel", "parallel", "reduction"]}
    ins(%A, %B : tensor<4x8xf64>, tensor<8x4xf64>)
    outs(%C : tensor<4x4xf64>) {
      ^bb0(%a: f64, %b: f64, %c: f64):
        %mul = arith.mulf %a, %b : f64
        %add = arith.addf %mul, %c : f64
        linalg.yield %add : f64
    }
  -> tensor<4x4xf64>
  func.return %C.out : tensor<4x4xf64>
}
