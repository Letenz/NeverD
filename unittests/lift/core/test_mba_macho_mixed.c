typedef unsigned int u32;

__attribute__((noinline, target("arm"))) u32 arm_mba(u32 a, u32 b) {
  return (a ^ b) + 2u * (a & b);
}

__attribute__((noinline, target("thumb"))) u32 thumb_mba(u32 a, u32 b) {
  return (a ^ b) + 2u * (a & b);
}

__attribute__((noinline, target("thumb"))) u32 thumb_calls_arm(u32 a, u32 b) {
  return arm_mba(a, b);
}

__attribute__((noinline, target("arm"))) u32 arm_calls_thumb(u32 a, u32 b) {
  return thumb_mba(a, b);
}

__attribute__((noinline, target("thumb"))) u32
thumb_calls_arm_calls_thumb(u32 a, u32 b) {
  return arm_calls_thumb(a, b);
}
