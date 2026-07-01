#!/usr/bin/env bash
set -euo pipefail

if ! grep -q 'kPresetParamEpsilon' NeuralAmpModeler/NeuralAmpModeler.cpp; then
  python3 .github/scripts/apply_beta3b.py
fi

git diff --check -- NeuralAmpModeler/NeuralAmpModeler.cpp NeuralAmpModeler/NeuralAmpModeler.h
