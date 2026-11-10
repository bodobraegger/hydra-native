// The P5 global of the web editor: a p5.js 1.6.0 instance in 2D mode whose canvas is a source for hydra,
// as in `p5 = new P5(); s0.init({ src: p5.canvas })`. p5.js loads on the first use of P5, from build/p5.min.js
// (the npm package p5@1.6.0, LGPL-2.1). The canvas is the 2D canvas of js/canvas.js.

// From src/lib/p5-wrapper.js of the hydra web editor (AGPL-3.0-or-later); the class body is unchanged,
// only the base class comes from the loaded p5.js.
function createP5Class (p5) {
  return class P5 extends p5 {
    constructor ({
      width = window.innerWidth,
      height = window.innerHeight,
      mode = 'P2D'
    } = {}) {
      super((p) => {
        p.setup = () => { p.createCanvas(width, height, p[mode]) }
        p.draw = () => { }
      }, 'hydra-ui')
      this.width = width
      this.height = height
      this.mode = mode
      this.canvas.style.position = 'absolute'
      this.canvas.style.top = '0px'
      this.canvas.style.left = '0px'
      this.canvas.style.zIndex = -1
    }

    show () {
      this.canvas.style.visibility = 'visible'
    }

    hide () {
      this.canvas.style.visibility = 'hidden'
    }

    // p5 clear function not covering canvas
    clear () {
      this.drawingContext.clearRect(0, 0, this.canvas.width, this.canvas.height)
    }
  }
}

let P5Class

Object.defineProperty(window, 'P5', {
  configurable: true,
  get () {
    if (!P5Class) {
      globalThis.eval(native.readFile(native.directory + '/p5.min.js'))
      P5Class = createP5Class(window.p5)
    }
    return P5Class
  }
})
