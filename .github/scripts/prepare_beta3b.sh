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

git diff --numstat -- NeuralAmpModeler/NeuralAmpModeler.cpp NeuralAmpModeler/NeuralAmpModeler.h >> beta3b-patch.log 2>&1

echo "patch_status=${patch_status}" >> beta3b-patch.log
cat beta3b-patch.log

exit "$patch_status"
