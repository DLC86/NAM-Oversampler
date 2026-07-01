#!/usr/bin/env bash
set -uo pipefail

: > beta3b-patch.log
if ! grep -q 'kPresetParamEpsilon' NeuralAmpModeler/NeuralAmpModeler.cpp; then
  python3 .github/scripts/apply_beta3b.py >> beta3b-patch.log 2>&1
  patch_status=$?
else
  patch_status=0
  echo "Patch marker already present" >> beta3b-patch.log
fi

git diff --check -- NeuralAmpModeler/NeuralAmpModeler.cpp NeuralAmpModeler/NeuralAmpModeler.h >> beta3b-patch.log 2>&1
diff_status=$?

echo "patch_status=${patch_status}" >> beta3b-patch.log
echo "diff_status=${diff_status}" >> beta3b-patch.log
cat beta3b-patch.log

if [ "$patch_status" -ne 0 ] || [ "$diff_status" -ne 0 ]; then
  exit 1
fi
