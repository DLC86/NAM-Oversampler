from pathlib import Path
import re


def read(path):
    with Path(path).open('r', encoding='utf-8', newline='') as f:
        return f.read()


def write(path, text):
    with Path(path).open('w', encoding='utf-8', newline='') as f:
        f.write(text)


def replace_block(text, pattern, replacement, label):
    m = re.search(pattern, text, flags=re.S)
    if not m:
        raise SystemExit(f'{label} not found')
    nl = '\r\n' if '\r\n' in m.group(0) else '\n'
    repl = replacement.replace('\n', nl)
    return text[:m.start()] + repl + text[m.end():]


path = 'NeuralAmpModeler/NeuralAmpModeler.h'
s = read(path)

s = replace_block(
    s,
    r'''  bool IsMidiLearnArmedForParam\(int paramIdx\) const;\r?\n  iplug::igraphics::IColor GetThemeColor\(\) const;\r?\n  void SetThemeColor\(const iplug::igraphics::IColor& color\);''',
    '''  bool IsMidiLearnArmedForParam(int paramIdx) const;
#if PLUG_HAS_UI
  iplug::igraphics::IColor GetThemeColor() const;
  void SetThemeColor(const iplug::igraphics::IColor& color);
#endif''',
    'theme declarations')

s = replace_block(
    s,
    r'''  WDL_String mHighLightColor;\r?\n  iplug::igraphics::IColor mThemeColor;''',
    '''  WDL_String mHighLightColor;
#if PLUG_HAS_UI
  iplug::igraphics::IColor mThemeColor = PluginColors::NAM_THEMECOLOR;
#endif''',
    'theme member')

write(path, s)
