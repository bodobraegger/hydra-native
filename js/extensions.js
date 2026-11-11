// The extensions panel of the web editor (src/stores/extension-store.js): three categories from the
// hydra-synth/hydra-extensions repository. src/ui.c draws the panel and calls the window.__* functions.
// HYDRA_EXTENSIONS_URL replaces the base URL, for tests with a local server.
import { pendingFetches } from './network.js'

const BASE_URL = native.environment('HYDRA_EXTENSIONS_URL') ||
  'https://raw.githubusercontent.com/hydra-synth/hydra-extensions/main/'
const CATEGORIES = [
  { name: 'extensions', slug: 'extensions' },
  { name: 'external libraries', slug: 'external-libraries' },
  { name: 'examples', slug: 'examples' }
].map((category) => ({ ...category, entries: [], state: 'new' }))

let selectedCategory = 0

function loadCategory (index) {
  const category = CATEGORIES[index]
  if (category.state !== 'new' && category.state !== 'failed') return
  category.state = 'loading'
  pendingFetches.set(native.fetchStart(BASE_URL + category.slug + '.json'), (ok, text) => {
    try {
      if (!ok) throw new Error('no answer')
      const entries = JSON.parse(text)
      if (!Array.isArray(entries)) throw new Error('no list')
      category.entries = entries.filter((entry) => entry && typeof entry === 'object')
      category.state = 'loaded'
    } catch (error) {
      category.state = 'failed'
    }
  })
}

// The panel as lines: "category<TAB>name<TAB>1 when selected" for each category, then
// "state<TAB>loading|failed|loaded", then per entry
// "entry<TAB>name<TAB>author<TAB>description<TAB>example count<TAB>thumbnail URL".
window.__extensionsPanel = (selectedText) => {
  if (selectedText !== '') selectedCategory = Math.max(0, Math.min(CATEGORIES.length - 1, Number(selectedText)))
  loadCategory(selectedCategory)
  const category = CATEGORIES[selectedCategory]
  const clean = (text) => String(text || '').replace(/[\t\n]+/g, ' ')
  return [
    ...CATEGORIES.map((item, index) => `category\t${item.name}\t${index === selectedCategory ? 1 : 0}`),
    `state\t${category.state}`,
    ...category.entries.map((entry) => ['entry', clean(entry.name), clean(entry.author), clean(entry.description),
      Array.isArray(entry.examples) ? entry.examples.length : 0,
      entry.thumbnail ? clean(BASE_URL + 'thumbnails/' + entry.thumbnail) : ''].join('\t'))
  ].join('\n')
}

const entry = (index) => CATEGORIES[selectedCategory].entries[Number(index)]

// "add <entry>": the load code of the entry ('extensions: add to editor').
window.__extensionLoadCode = (index) => {
  const item = entry(index)
  if (!item || typeof item.load !== 'string') throw new Error('no load code')
  native.sandbox() // the load code comes from the extension list on the network
  return item.load
}

// "<entry> <example>": the code of an example URL ('extensions: load example').
window.__extensionExample = (indexes) => {
  const [index, example] = indexes.split(' ').map(Number)
  const item = entry(index)
  if (!item || !Array.isArray(item.examples) || !item.examples[example]) throw new Error('no example')
  return window.__codeFromUrl(String(item.examples[example]))
}

window.__extensionDocumentation = (index) => {
  const item = entry(index)
  // src/ui.c opens the answer in the browser, and the list comes from the network: only a web address passes.
  if (!item || !/^https?:\/\/\S+$/.test(item.documentation)) throw new Error('no documentation')
  return item.documentation
}
