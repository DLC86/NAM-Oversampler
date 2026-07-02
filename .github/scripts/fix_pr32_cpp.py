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


path = 'NeuralAmpModeler/NeuralAmpModeler.cpp'
s = read(path)

s = replace_block(
    s,
    r'''\r?\n  // SetThemeColor\(PluginColors::NAM_THEMECOLOR\);.*?\r?\n  \}\);\r?\n(?=\})''',
    '''
#if PLUG_HAS_UI
  if (auto* pGraphics = GetUI())
  {
    if (GetParam(kFollowTrackColor)->Bool())
    {
      int r = 0, g = 0, b = 0;
      GetTrackColor(r, g, b);
      if (r + g + b > 0)
        SetThemeColor(IColor(255, r, g, b));
    }
    else if (mHighLightColor.GetLength())
      SetThemeColor(IColor::FromColorCodeStr(mHighLightColor.Get()));
    else
      SetThemeColor(PluginColors::NAM_THEMECOLOR);

    pGraphics->ForStandardControlsFunc([&](IControl* pControl) {
      if (auto* pVectorBase = pControl->As<IVectorBase>())
      {
        pVectorBase->SetColor(kX1, GetThemeColor());
        pVectorBase->SetColor(kPR, GetThemeColor().WithOpacity(0.6f));
        pVectorBase->SetColor(kFR, GetThemeColor().WithOpacity(0.1f));
        pVectorBase->SetColor(kX3, GetThemeColor().WithContrast(0.1f));
        pVectorBase->SetColor(kOFF, GetThemeColor().WithOpacity(0.1f));
      }
    });
    pGraphics->SetAllControlsDirty();
  }
#endif
''',
    'OnIdle theme block')

s = replace_block(
    s,
    r'''bool NeuralAmpModeler::SerializeState\(IByteChunk& chunk\) const\r?\n\{.*?\r?\n\}\r?\n\r?\n(?=int NeuralAmpModeler::UnserializeState)''',
    '''bool NeuralAmpModeler::SerializeState(IByteChunk& chunk) const
{
  WDL_String header("###NeuralAmpModeler###");
  chunk.PutStr(header.Get());
  WDL_String version(PLUG_VERSION_STR);
  chunk.PutStr(version.Get());
  chunk.PutStr(mNAMPath.Get());
  chunk.PutStr(mIRPath.Get());

  const bool paramsSerialized = SerializeParams(chunk);
  if (paramsSerialized)
  {
    chunk.PutStr(_SerializeToneStackComponentState().c_str());
    chunk.PutStr(_SerializeInternalPresetState().c_str());
    chunk.PutStr(mHighLightColor.Get());
  }
  return paramsSerialized;
}

''',
    'SerializeState')

s = replace_block(
    s,
    r'''iplug::igraphics::IColor NeuralAmpModeler::GetThemeColor\(\) const\r?\n\{.*?\r?\n\}\r?\n\r?\nvoid NeuralAmpModeler::SetThemeColor\(const iplug::igraphics::IColor& color\)\r?\n\s*\{ mThemeColor = color; \}''',
    '''#if PLUG_HAS_UI
iplug::igraphics::IColor NeuralAmpModeler::GetThemeColor() const
{
  return mThemeColor;
}

void NeuralAmpModeler::SetThemeColor(const iplug::igraphics::IColor& color)
{
  mThemeColor = color;
}
#endif''',
    'theme accessors')

pattern = r'''(    case kChannelMode:\r?\n    case kMidiChannel:\r?\n)(      return false;)'''
s, n = re.subn(pattern, r'\1    case kFollowTrackColor:\n\2', s, count=1)
if n != 1:
    raise SystemExit('Internal preset exclusion not found')

write(path, s)
