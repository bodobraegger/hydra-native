// The languages of the web editor: English is built in (text-elements.js), the others come from the
// hydra-synth/l10n repository like in language-store.js, with English as the fallback. Downloads stay in
// ~/.cache/hydra-native/l10n, so that a language also works offline after its first download.
// src/ui.c calls the window.__* functions through the `call` host function.
import english from './hydra-editor/text-elements.js'

const LANGUAGES = ['ja', 'es', 'ar', 'id', 'pt-br', 'ch', 'ko', 'fr', 'de']
const RIGHT_TO_LEFT = ['ar']
const languagePath = (language) => `https://raw.githubusercontent.com/hydra-synth/l10n/main/${language}/editor.json`
// The link targets of src/views/info.js.
const LINKS = {
  author: 'https://ojack.xyz',
  docs: 'https://hydra.ojack.xyz/docs',
  functions: 'https://hydra.ojack.xyz/api',
  gallery: 'https://twitter.com/hydra_patterns',
  repo: 'https://github.com/hydra-synth/hydra',
  pixeljam: 'http://pixeljam.glitch.me/',
  garden: 'https://hydra.ojack.xyz/garden',
  'hydra-book': 'https://hydra-book.glitch.me/',
  tutorials: 'https://github.com/ojack/hydra/blob/master/examples/README.md',
  discord: 'https://discord.gg/ZQjfHkNHXC',
  facebook: 'https://www.facebook.com/groups/1084288351771117/',
  'open-collective': 'https://opencollective.com/hydra-synth'
}

// Translations are stored under the first part of the code, as i18next does in the web editor ("pt-br" as "pt").
const resources = { en: english.en.translation }
const key = (language) => language.split('-')[0]
let selected = 'en'

const cacheDirectory = () => `${native.environment('HOME') || '/tmp'}/.cache/hydra-native/l10n`

function addLanguage (language, text) {
  try {
    resources[key(language)] = JSON.parse(text)
    return true
  } catch (error) {
    console.warn(`l10n: ${language} is no JSON: ${error}`)
    return false
  }
}

export function startLanguages (fetchCallbacks, requested, customUrl) {
  for (const language of LANGUAGES) {
    try {
      addLanguage(language, native.readFile(`${cacheDirectory()}/${language}.json`))
    } catch {}
    fetchCallbacks.set(native.fetchStart(languagePath(language)), (ok, text) => {
      if (ok && addLanguage(language, text)) native.writeFile(`${cacheDirectory()}/${language}.json`, text)
    })
  }
  if (requested && customUrl) {
    fetchCallbacks.set(native.fetchStart(customUrl), (ok, text) => { if (ok) addLanguage(requested, text) })
  }
  // Like the language detector of the web editor: the requested language, else the one of the system.
  const system = (native.environment('LANG') || 'en').split(/[._]/)[0].toLowerCase()
  selected = key(requested || system)
}

function lookup (language, path) {
  return path.split('.').reduce((value, part) => (value == null ? undefined : value[part]), resources[language])
}

export function translate (path) {
  const value = lookup(selected, path)
  return value === undefined ? lookup('en', path) : value
}

// "<a {{docs}}>the documentation</a>" becomes "the documentation (https://hydra.ojack.xyz/docs)".
function plain (text) {
  return String(text)
    .replace(/<a\s*\{\{([\w-]+)\}\}\s*>(.*?)<\/a>/g, (match, name, label) => `${label.trim()} (${LINKS[name] || ''})`)
    .replace(/<[^>]+>/g, '')
    .replace(/\s+/g, ' ')
    .trim()
}

// ---------------------------------------------------------------- right-to-left text

// Presentation forms of the Arabic letters U+0621 to U+064A: [isolated, final, initial, medial]; two forms for
// letters that join only to the letter before them.
const ARABIC_FORMS = {
  0x0621: [0xFE80], 0x0622: [0xFE81, 0xFE82], 0x0623: [0xFE83, 0xFE84], 0x0624: [0xFE85, 0xFE86],
  0x0625: [0xFE87, 0xFE88], 0x0626: [0xFE89, 0xFE8A, 0xFE8B, 0xFE8C], 0x0627: [0xFE8D, 0xFE8E],
  0x0628: [0xFE8F, 0xFE90, 0xFE91, 0xFE92], 0x0629: [0xFE93, 0xFE94], 0x062A: [0xFE95, 0xFE96, 0xFE97, 0xFE98],
  0x062B: [0xFE99, 0xFE9A, 0xFE9B, 0xFE9C], 0x062C: [0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0],
  0x062D: [0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4], 0x062E: [0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8], 0x062F: [0xFEA9, 0xFEAA],
  0x0630: [0xFEAB, 0xFEAC], 0x0631: [0xFEAD, 0xFEAE], 0x0632: [0xFEAF, 0xFEB0],
  0x0633: [0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4], 0x0634: [0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8],
  0x0635: [0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC], 0x0636: [0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0],
  0x0637: [0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4], 0x0638: [0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8],
  0x0639: [0xFEC9, 0xFECA, 0xFECB, 0xFECC], 0x063A: [0xFECD, 0xFECE, 0xFECF, 0xFED0],
  0x0641: [0xFED1, 0xFED2, 0xFED3, 0xFED4], 0x0642: [0xFED5, 0xFED6, 0xFED7, 0xFED8],
  0x0643: [0xFED9, 0xFEDA, 0xFEDB, 0xFEDC], 0x0644: [0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0],
  0x0645: [0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4], 0x0646: [0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8],
  0x0647: [0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC], 0x0648: [0xFEED, 0xFEEE], 0x0649: [0xFEEF, 0xFEF0],
  0x064A: [0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4]
}
const TATWEEL = 0x0640
const isArabic = (code) => (code >= 0x0600 && code <= 0x06FF) || (code >= 0xFE70 && code <= 0xFEFF)
const isMark = (code) => code >= 0x064B && code <= 0x0652
const joinsForward = (code) => code === TATWEEL || (ARABIC_FORMS[code] && ARABIC_FORMS[code].length === 4)
const joinsBackward = (code) => code === TATWEEL || Boolean(ARABIC_FORMS[code])

// A basic Arabic shaper without ligatures: each letter takes the form for its neighbors.
function shapeArabic (codes) {
  const neighbor = (index, step) => {
    for (let i = index + step; i >= 0 && i < codes.length; i += step) if (!isMark(codes[i])) return codes[i]
    return 0
  }
  return codes.map((code, index) => {
    const forms = ARABIC_FORMS[code]
    if (!forms || forms.length === 1) return forms ? forms[0] : code
    const before = joinsForward(neighbor(index, -1))
    const after = forms.length === 4 && joinsBackward(neighbor(index, 1))
    return forms[before ? (after ? 3 : 1) : (after ? 2 : 0)]
  })
}

// Visual order of one line of right-to-left text: the runs reverse, and the letters of Arabic runs too.
function visualOrder (line) {
  const codes = shapeArabic([...line].map((character) => character.codePointAt(0)))
  const runs = []
  for (const code of codes) {
    const rtl = isArabic(code) || (code === 0x20 && runs.length && runs[runs.length - 1].rtl)
    if (!runs.length || runs[runs.length - 1].rtl !== rtl) runs.push({ rtl, codes: [] })
    runs[runs.length - 1].codes.push(code)
  }
  return runs.reverse().map((run) => String.fromCodePoint(...(run.rtl ? run.codes.reverse() : run.codes))).join('')
}

// ---------------------------------------------------------------- the info panel

const columnsOf = (text) => [...text].reduce((sum, character) => {
  const code = character.codePointAt(0)
  const wide = (code >= 0x1100 && code <= 0x115F) || (code >= 0x2E80 && code <= 0xA4CF) || (code >= 0xAC00 && code <= 0xD7A3) ||
    (code >= 0xF900 && code <= 0xFAFF) || (code >= 0xFF00 && code <= 0xFF60)
  return sum + (wide ? 2 : 1)
}, 0)

// Breaks at spaces; scripts without spaces (Japanese, Chinese) break at any character.
function wrap (text, columns) {
  const lines = []
  let line = ''
  for (const word of text.split(/(?<=\s)|(?=[　-鿿가-힣])/u)) {
    if (line && columnsOf(line + word) > columns) {
      lines.push(line.trimEnd())
      line = word.trimStart()
    } else {
      line += word
    }
  }
  if (line) lines.push(line.trimEnd())
  return lines
}

// The content of src/views/info.js as lines of at most `columns` columns. The first line is "ltr" or "rtl".
window.__infoText = (columnsText) => {
  const columns = Math.max(Number(columnsText) || 60, 20)
  const t = translate
  const rule = '/'.repeat(Math.min(59, columns))
  const paragraphs = [
    `${t('info.title')}: ${t('info.subtitle')}`, rule, t('info.description'), t('info.get-started-title'),
    ...(t('info.get-started-list') || []).map((item, index) => `${index + 1}. ${item}`), rule,
    t('info.description-detailed'), t('info.uses'), ...(t('info.uses-list') || []).map((item) => `- ${item}`),
    t('info.author'), t('info.more-info'), t('info.more-info-forums'), t('info.support')
  ].map(plain)
  const rtl = RIGHT_TO_LEFT.includes(selected)
  const lines = paragraphs.flatMap((paragraph) => [...wrap(paragraph, columns), ''])
  return [rtl ? 'rtl' : 'ltr', ...(rtl ? lines.map(visualOrder) : lines)].join('\n')
}

// One line per language: "code<TAB>name<TAB>1 when selected".
window.__languages = () => ['en', ...LANGUAGES].filter((language) => resources[key(language)])
  .map((language) => {
    const name = lookup(key(language), 'language-name') || language
    return `${key(language)}\t${RIGHT_TO_LEFT.includes(key(language)) ? visualOrder(name) : name}\t${key(language) === selected ? 1 : 0}`
  }).join('\n')

window.__setLanguage = (language) => {
  if (resources[key(language)]) selected = key(language)
  return selected
}

// The tooltip of a toolbar button in the selected language (toolbar.run, toolbar.clear, ...).
window.__translate = (path) => {
  const text = plain(translate(path) || '')
  return RIGHT_TO_LEFT.includes(selected) ? visualOrder(text) : text
}
