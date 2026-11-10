// The host functions of src/main.c. esbuild injects this binding into each module that uses `native`, and the
// global goes away, so that sketches cannot reach them (file and shell access). The tests keep the global.
export const native = globalThis.native
if (!native.environment('HYDRA_NATIVE_TEST')) delete globalThis.native
