from pathlib import Path
import re

path = Path("NeuralAmpModeler/Unserialization.cpp")
with path.open("r", encoding="utf-8", newline="") as f:
    text = f.read()

apply_pattern = re.compile(
    r'(void NeuralAmpModeler::_UnserializeApplyConfig\(nlohmann::json& config\)\r?\n'
    r'\{\r?\n'
    r'  mApplyingInternalPreset\.store\(true, std::memory_order_release\);\r?\n)'
)

def add_common_default(match):
    block = match.group(1)
    nl = "\r\n" if "\r\n" in block else "\n"
    return block + (
        '  if (!config.contains("followTrackColor"))' + nl
        + '    config["followTrackColor"] = 0.0;' + nl
    )

text, count = apply_pattern.subn(add_common_default, text, count=1)
if count != 1:
    raise RuntimeError("Unexpected _UnserializeApplyConfig layout")

v16_pattern = re.compile(
    r'(void _UpdateConfigFrom_1_6_0\(nlohmann::json& config\).*?'
    r'  if \(!config\.contains\("Input Boost"\)\)\r?\n'
    r'    config\["Input Boost"\] = 0\.0;\r?\n)'
    r'  if \(!config\.contains\("followTrackColor"\)\)\r?\n'
    r'    config\["followTrackColor"\] = 0\.0;\r?\n',
    re.S,
)
text, count = v16_pattern.subn(r'\1', text, count=1)
if count != 1:
    raise RuntimeError("Unexpected _UpdateConfigFrom_1_6_0 layout")

legacy_pattern = re.compile(
    r'(int _GetConfigFrom_0_7_14\(.*?'
    r'std::vector<std::string> paramNames\{.*?'
    r'"OutputMode",\r?\n)'
    r'\s*"followTrackColor",\r?\n'
    r'(\s*"Slim"\};)',
    re.S,
)
text, count = legacy_pattern.subn(r'\1\2', text, count=1)
if count != 1:
    raise RuntimeError("Legacy v0.7.14 parameter layout was not found")

with path.open("w", encoding="utf-8", newline="") as f:
    f.write(text)
