// CSS colors for the audio meter and the text sources: hex, rgb() and rgba(), and the basic color names.
const NAMED_COLORS = {
  black: '000', silver: 'c0c0c0', gray: '808080', grey: '808080', white: 'fff', maroon: '800000', red: 'f00',
  purple: '800080', fuchsia: 'f0f', magenta: 'f0f', green: '008000', lime: '0f0', olive: '808000', yellow: 'ff0',
  navy: '000080', blue: '00f', teal: '008080', aqua: '0ff', cyan: '0ff', orange: 'ffa500', pink: 'ffc0cb'
}

function hexColor (hex) {
  if (hex.length <= 4) hex = [...hex].map((digit) => digit + digit).join('')
  return [0, 2, 4, 6].map((offset) => parseInt(hex.slice(offset, offset + 2) || 'ff', 16) / 255)
}

// Returns [r, g, b, a] in 0..1, or null for a value that is no such color.
export function parseColor (value) {
  const text = String(value).trim().toLowerCase()
  if (text === 'transparent') return [0, 0, 0, 0]
  if (NAMED_COLORS[text]) return hexColor(NAMED_COLORS[text])
  if (/^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/.test(text)) return hexColor(text.slice(1))
  const match = /^rgba?\(([^)]*)\)$/.exec(text)
  if (!match) return null
  const parts = match[1].split(/[\s,/]+/).filter(Boolean)
  const channel = (part, scale) => part.endsWith('%') ? parseFloat(part) / 100 : parseFloat(part) / scale
  return [...parts.slice(0, 3).map((part) => channel(part, 255)), parts.length > 3 ? channel(parts[3], 1) : 1]
}
