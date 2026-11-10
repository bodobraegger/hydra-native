// Replaces the repl module of the web editor for Mutator.js: evaluates in the global scope and reports errors.
export default {
  eval (code, callback = () => {}) {
    try {
      globalThis.eval(code)
      callback(code, false)
    } catch (error) {
      console.log(`mutator: ${error}`)
      callback(code, true)
    }
  }
}
