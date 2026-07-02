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


path = 'NeuralAmpModeler/Unserialization.cpp'
s = read(path)

s = replace_block(
    s,
    r'''  mIRPath\.Set\(static_cast<std::string>\(config\["IRPath"\]\)\.c_str\(\)\);\r?\n  mHighLightColor\.Set\(static_cast<std::string>\(config\["HighLightColor"\]\)\.c_str\(\)\);''',
    '''  mIRPath.Set(static_cast<std::string>(config["IRPath"]).c_str());
  if (config.contains("HighLightColor") && config["HighLightColor"].is_string())
    mHighLightColor.Set(config["HighLightColor"].get<std::string>().c_str());
  else
    mHighLightColor.Set("");''',
    'highlight restore')

s = replace_block(
    s,
    r'''  pos = chunk\.GetStr\(path, pos\);\r?\n  config\["IRPath"\] = std::string\(path\.Get\(\)\);\r?\n  pos = chunk\.GetStr\(path, pos\);\r?\n  config\["HighLightColor"\] = std::string\(path\.Get\(\)\);''',
    '''  pos = chunk.GetStr(path, pos);
  config["IRPath"] = std::string(path.Get());''',
    'legacy path layout')

parser = '''int _GetConfigFrom_2_0_1(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  int pos = _GetConfigFrom_1_6_0(chunk, startPos, config);
  _UpdateConfigFrom_1_6_0(config);

  auto tryReadJsonString = [&](int readPos, nlohmann::json& value) {
    WDL_String serialized;
    const int nextPos = chunk.GetStr(serialized, readPos);
    if (nextPos < 0)
      return -1;
    try
    {
      value = nlohmann::json::parse(serialized.Get());
      return nextPos;
    }
    catch (...)
    {
      return -1;
    }
  };

  for (int extraParamCount = 3; extraParamCount >= 0; --extraParamCount)
  {
    int readPos = pos;
    double extraValues[3] = {0.0, 0.0, 0.0};
    bool valid = true;
    for (int i = 0; i < extraParamCount; ++i)
    {
      const int nextPos = chunk.Get(&extraValues[i], readPos);
      if (nextPos < 0)
      {
        valid = false;
        break;
      }
      readPos = nextPos;
    }
    if (!valid)
      continue;

    nlohmann::json toneStackState;
    const int posAfterToneStack = tryReadJsonString(readPos, toneStackState);
    if (posAfterToneStack < 0)
      continue;

    if (extraParamCount >= 1)
      config["Input Boost"] = extraValues[0];
    if (extraParamCount >= 2)
      config["MIDI Channel"] = extraValues[1];
    if (extraParamCount >= 3)
      config["followTrackColor"] = extraValues[2];

    config["ToneStack Components"] = toneStackState;
    int finalPos = posAfterToneStack;

    nlohmann::json internalPresetState;
    const int posAfterInternalPresets = tryReadJsonString(finalPos, internalPresetState);
    if (posAfterInternalPresets >= 0)
    {
      config["Internal Presets"] = internalPresetState;
      finalPos = posAfterInternalPresets;
    }

    WDL_String highlightColor;
    const int posAfterHighlight = chunk.GetStr(highlightColor, finalPos);
    if (posAfterHighlight >= 0)
    {
      config["HighLightColor"] = std::string(highlightColor.Get());
      finalPos = posAfterHighlight;
    }

    return finalPos;
  }

  return pos;
}

// v1.6.0'''

s = replace_block(
    s,
    r'''int _GetConfigFrom_2_0_1\(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config\)\r?\n\{.*?\r?\n\}\r?\n\r?\n// v1\.6\.0''',
    parser,
    'current parser')

s = replace_block(
    s,
    r'''  if \(!config\.contains\("Input Boost"\)\)\r?\n    config\["Input Boost"\] = 0\.0;''',
    '''  if (!config.contains("Input Boost"))
    config["Input Boost"] = 0.0;
  if (!config.contains("followTrackColor"))
    config["followTrackColor"] = 0.0;''',
    'follow default')

s = re.sub(r',\r?\n\s*"followTrackColor"(?=\r?\n|\};)', '', s)
s = s.replace(', "followTrackColor"};', '};')

write(path, s)
