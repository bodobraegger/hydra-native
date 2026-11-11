// The languages of the web editor: English is built in (text-elements.js), the others come from the
// hydra-synth/l10n repository like in language-store.js, with English as the fallback. Downloads stay in
// ~/.cache/hydra-native/l10n, so that a language also works offline after its first download.
// src/ui.c calls the window.__* functions through the `call` host function.
import english from './hydra-editor/text-elements.js'
import { pendingFetches } from './network.js'

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
// Without a prototype, so that a language code such as "constructor" finds nothing.
const resources = Object.assign(Object.create(null), { en: english.en.translation })
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

for (const language of LANGUAGES) {
  try {
    addLanguage(language, native.readFile(`${cacheDirectory()}/${language}.json`))
  } catch {}
  pendingFetches.set(native.fetchStart(languagePath(language)), (ok, text) => {
    if (ok && addLanguage(language, text)) native.writeFile(`${cacheDirectory()}/${language}.json`, text)
  })
}
if (native.language && native.l10nUrl) {
  pendingFetches.set(native.fetchStart(native.l10nUrl), (ok, text) => { if (ok) addLanguage(native.language, text) })
}
// Like the language detector of the web editor: the requested language, else the one of the system.
selected = key(native.language || (native.environment('LANG') || 'en').split(/[._]/)[0].toLowerCase())

function lookup (language, path) {
  return path.split('.').reduce((value, part) => (value == null ? undefined : value[part]), resources[language])
}

function translate (path) {
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

// The content of src/views/info.js, one paragraph per line in logical order; the first line is "ltr" or "rtl".
// src/ui.c wraps, shapes (HarfBuzz) and orders the text (bidi).
window.__infoText = () => {
  const t = translate
  const rule = '/'.repeat(59)
  const paragraphs = [
    `${t('info.title')}: ${t('info.subtitle')}`, rule, t('info.description'), t('info.get-started-title'),
    ...(t('info.get-started-list') || []).map((item, index) => `${index + 1}. ${item}`), rule,
    t('info.description-detailed'), t('info.uses'), ...(t('info.uses-list') || []).map((item) => `- ${item}`),
    t('info.author'), t('info.more-info'), t('info.more-info-forums'), t('info.support')
  ].map(plain)
  return [RIGHT_TO_LEFT.includes(selected) ? 'rtl' : 'ltr', ...paragraphs].join('\n')
}

// One line per language: "code<TAB>name<TAB>1 when selected".
window.__languages = () => ['en', ...LANGUAGES].filter((language) => resources[key(language)])
  .map((language) => {
    const name = plain(lookup(key(language), 'language-name') || language)
    return `${key(language)}\t${name}\t${key(language) === selected ? 1 : 0}`
  }).join('\n')

window.__setLanguage = (language) => {
  if (resources[key(String(language))]) selected = key(String(language))
  return selected
}

// The tooltip of a toolbar button in the selected language (toolbar.run, toolbar.clear, ...).
window.__translate = (path) => plain(translate(path) || '')
