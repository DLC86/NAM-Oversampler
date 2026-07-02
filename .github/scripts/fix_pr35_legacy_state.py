from pathlib import Path
import re

path = Path("NeuralAmpModeler/Unserialization.cpp")
with path.open("r", encoding="utf-8", newline="") as f:
    text = f.read()

newline = "\r\n" if "\r\n" in text else "\n"

apply_marker = (
    "void NeuralAmpModeler::_UnserializeApplyConfig(nlohmann::json& config)" + newline
    + "{" + newline
    + "  mApplyingInternalPreset.store(true, std::memory_order_release);" + newline
)
apply_replacement = apply_marker + (
    "  if (!config.contains(\"followTrackColor\"))" + newline
    + "    config[\"followTrackColor\"] = 0.0;" + newline
)
if text.count(apply_marker) != 1:
    raise RuntimeError("Unexpected _UnserializeApplyConfig layout")
text = text.replace(apply_marker, apply_replacement, 1)

redundant_default = (
    "  if (!config.contains(\"followTrackColor\"))" + newline
    + "    config[\"followTrackColor\"] = 0.0;" + newline
)
# Keep only the common default inserted above.
first = text.find(redundant_default)
second = text.find(redundant_default, first + len(redundant_default))
if first < 0 or second < 0:
    raise RuntimeError("Expected common and v1.6 defaults")
text = text[:second] + text[second + len(redundant_default):]

legacy_pattern = re.compile(
    r'(int _GetConfigFrom_0_7_14\(.*?std::vector<std::string> paramNames\{.*?"OutputMode",\r?\n)'
    r'\s*"followTrackColor",\r?\n'
    r'(\s*"Slim"\};)',
    re.S,
)
text, count = legacy_pattern.subn(r"\1\2", text, count=1)
if count != 1:
    raise RuntimeError("Legacy v0.7.14 parameter layout was not found")

with path.open("w", encoding="utf-8", newline="") as f:
    f.write(text)
