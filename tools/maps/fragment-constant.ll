source_filename = "constant-fragment"
target triple = "air64-apple-ios14.0.0"
define <{ <4 x float> }> @constant_fragment() {
 %r=insertvalue <{ <4 x float> }> undef, <4 x float> <float 1.0, float 2.0, float 3.0, float 4.0>, 0
 ret <{ <4 x float> }> %r
}
!air.version = !{!0}
!air.fragment = !{!1}
!0 = !{i32 2, i32 5, i32 0}
!1 = !{ptr @constant_fragment, !2, !4}
!2 = !{!3}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!4 = !{}
