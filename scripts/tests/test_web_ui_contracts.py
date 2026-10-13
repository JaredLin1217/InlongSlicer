"""Keep bundled web initialization, host dictionaries and catalogs in agreement."""
import ast
from html.parser import HTMLParser
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / 'resources/web'
PAGES = {
    **{f'guide/{page}/index.html': 'src/slic3r/GUI/WebGuideDialog.cpp'
       for page in ('0', '1', '11', '21', '22', '23', '24', '4inlong', '5')},
    'homepage/index.html': 'src/slic3r/GUI/WebViewDialog.cpp',
    'model/index.html': 'src/slic3r/GUI/Project.cpp',
}


class Page(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.scripts = []
        self.inline_scripts = []
        self.in_inline_script = False
        self.keys = set()
        self.legacy = []
        self.feed(text)

    def handle_starttag(self, tag, attributes):
        attributes = dict(attributes)
        if tag == 'script' and 'src' in attributes:
            self.scripts.append(attributes['src'])
        if tag == 'script' and 'src' not in attributes:
            self.in_inline_script = True
        if 'tid' in attributes:
            self.legacy.append(attributes['tid'])
        for name, value in attributes.items():
            if name == 'data-i18n' or name.startswith('data-i18n-'):
                self.keys.add(value)

    def handle_endtag(self, tag):
        if tag == 'script':
            self.in_inline_script = False

    def handle_data(self, data):
        if self.in_inline_script:
            self.inline_scripts.append(data)


def host_strings(path):
    source = path.read_text(encoding='utf-8')
    pattern = r'\{"([\w]+)",\s*_u8L(_CONTEXT)?\("((?:[^"\\]|\\.)*)"(?:,\s*"([^"]*)")?\)\}'
    return {key: (context or '', ast.literal_eval('"' + message + '"'))
            for key, _, message, context in re.findall(pattern, source)}


def po_strings(path):
    """Read singular gettext entries, including continued strings and contexts."""
    entries = {}
    for block in path.read_text(encoding='utf-8-sig').split('\n\n'):
        fields = {}
        field = None
        for line in block.splitlines():
            match = re.match(r'(msgctxt|msgid|msgstr) (".*")$', line)
            if match:
                field = match[1]
                fields[field] = ast.literal_eval(match[2])
            elif line.startswith('"') and field:
                fields[field] += ast.literal_eval(line)
        if 'msgid' in fields and not re.search(r'^#,.*\bfuzzy\b', block, re.M):
            entries[(fields.get('msgctxt', ''), fields['msgid'])] = fields.get('msgstr', '')
    return entries


class TestWebUiContracts(unittest.TestCase):
    def test_every_bundled_page_loads_existing_local_scripts(self):
        for name in PAGES:
            path = WEB / name
            page = Page(path.read_text(encoding='utf-8-sig'))
            with self.subTest(page=name):
                self.assertTrue(page.scripts)
                resolved = [(path.parent / src).resolve() for src in page.scripts]
                for script in resolved:
                    self.assertTrue(script.is_relative_to(WEB.resolve()), script)
                    self.assertTrue(script.is_file(), script)
                self.assertIn((WEB / 'include/i18n.js').resolve(), resolved)

    def test_active_markup_uses_the_host_translation_contract(self):
        for name, host in PAGES.items():
            path = WEB / name
            page = Page(path.read_text(encoding='utf-8-sig'))
            with self.subTest(page=name):
                self.assertEqual(page.legacy, [], 'Legacy tid is not translated by i18n.js')
                strings = host_strings(ROOT / host)
                self.assertTrue(page.keys <= strings.keys(), page.keys - strings.keys())

    def test_chinese_catalogs_cover_static_and_dynamic_web_labels(self):
        for locale in ('zh_CN', 'zh_TW'):
            translations = po_strings(ROOT / f'localization/i18n/{locale}/InlongSlicer_{locale}.po')
            for name, host in PAGES.items():
                path = WEB / name
                page = Page(path.read_text(encoding='utf-8-sig'))
                keys = set(page.keys)
                for src in page.scripts:
                    script = path.parent / src
                    if script.is_file():
                        keys.update(re.findall(r'\bT\("([\w]+)"', script.read_text(encoding='utf-8-sig')))
                strings = host_strings(ROOT / host)
                for key in sorted(keys):
                    with self.subTest(locale=locale, page=name, key=key):
                        self.assertIn(key, strings)
                        translated = translations.get(strings[key], '')
                        self.assertTrue(translated, f'Missing Chinese translation: {strings[key]}')
                        self.assertEqual(translated.count('%s'), strings[key][1].count('%s'))

    def test_page_wrappers_do_not_override_shared_wizard_logic(self):
        for number, shared in (('21', '21'), ('24', '21'), ('22', '22'), ('23', '22')):
            path = WEB / f'guide/{number}/index.html'
            page = Page(path.read_text(encoding='utf-8-sig'))
            common = WEB / f'guide/{shared}/common.js'
            with self.subTest(page=number):
                self.assertIn(common.resolve(), [(path.parent / src).resolve() for src in page.scripts])
                functions = lambda text: set(re.findall(r'^\s*function\s+(\w+)\s*\(', text, re.M))
                shared_functions = functions(common.read_text(encoding='utf-8-sig'))
                page_functions = functions((path.parent / f'{number}.js').read_text(encoding='utf-8-sig'))
                page_functions.update(functions('\n'.join(page.inline_scripts)))
                self.assertEqual(shared_functions & page_functions, set())

    def test_wizard_keeps_the_inlong_stealth_page(self):
        source = (WEB / 'guide/22/22.js').read_text(encoding='utf-8-sig')
        self.assertIn('../4inlong/index.html', source)
        self.assertNotIn('../4orca/index.html', source)


if __name__ == '__main__':
    unittest.main()
