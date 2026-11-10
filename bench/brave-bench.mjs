// Usage: node bench/brave-bench.mjs <sketch.js> [width height] [--vsync] [--headed]
// Playwright is not a dependency of hydra-native: PLAYWRIGHT_MODULE can point to the index.mjs of an installed copy.
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE ?? 'playwright')
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const MEASURE_SECONDS = 10
const WARMUP_MS = 3000
const args = process.argv.slice(2).filter((arg) => !arg.startsWith('--'))
const [sketchPath, width = '1920', height = '1080'] = args
const vsync = process.argv.includes('--vsync')
const headed = process.argv.includes('--headed')
const here = path.dirname(fileURLToPath(import.meta.url))
const prelude = readFileSync(path.join(here, 'prelude.js'), 'utf8')
const sketch = sketchPath ? readFileSync(sketchPath, 'utf8') : 'osc().out()'

const flags = ['--ignore-gpu-blocklist', '--enable-gpu', '--use-angle=gl', '--ozone-platform=wayland']
if (!vsync) flags.push('--disable-gpu-vsync', '--disable-frame-rate-limit')
const browser = await chromium.launch({ executablePath: '/usr/bin/brave-browser', headless: !headed, args: flags })
try {
  const page = await browser.newPage({ viewport: { width: Number(width), height: Number(height) } })
  page.on('console', (message) => { if (message.type() === 'error') console.error('page:', message.text()) })
  page.on('pageerror', (error) => console.error('page error:', error.message))
  await page.goto(`file://${here}/bench.html?w=${width}&h=${height}`)
  await page.evaluate(([pre, code]) => { hydra.eval(pre); hydra.eval(code) }, [prelude, sketch])
  await page.waitForTimeout(WARMUP_MS)
  const result = await page.evaluate((seconds) => window.measure(seconds), MEASURE_SECONDS)
  const renderer = await page.evaluate(() => window.renderer)
  console.log(JSON.stringify({ sketch: path.basename(sketchPath || 'osc'), width, height, vsync, headed, renderer, ...result }))
} finally {
  await browser.close()
}
