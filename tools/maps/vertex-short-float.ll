source_filename = "short-float-input"
target triple = "air64-apple-ios14.0.0"
define <{ <4 x float> }> @short_float(<4 x float> %p) {
 %r=insertvalue <{ <4 x float> }> undef, <4 x float> %p, 0
 ret <{ <4 x float> }> %r
}
!air.version = !{!0}
!air.vertex = !{!1}
!0 = !{i32 2, i32 5, i32 0}
!1 = !{ptr @short_float, !2, !4}
!2 = !{!3}
!3 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!4 = !{!5}
!5 = !{i32 0, !"air.vertex_input", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
