#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

file='.cache/models/umxl-vocals.onnx'
weights='.cache/models/umxl-vocals.pth'
url='https://zenodo.org/records/5069601/files/vocals-bccbd9aa.pth'
sha='bccbd9aa5414a5a1c869277dd9e44f1522beca0b2b2639a84fe4eeb911382fe6'
venv='.cache/umx-export'
mkdir -p .cache/models

if [[ -f "$file" ]]; then
  printf 'Clean Sing model already present: %s\n' "$file"
  exit 0
fi
printf '%s\n' 'Open-Unmix UMX-L weights are CC BY-NC-SA 4.0 (non-commercial). They stay in .cache and are not bundled.'

verify() {
  printf '%s  %s\n' "$sha" "$1" | shasum -a 256 -c - >/dev/null
}
trap 'rm -f "$weights.partial" "$file.partial"' EXIT
if ! { [[ -f "$weights" ]] && verify "$weights"; }; then
  curl -fL --retry 3 "$url" -o "$weights.partial"
  verify "$weights.partial" || { echo 'Clean Sing weights checksum mismatch' >&2; exit 1; }
  mv "$weights.partial" "$weights"
fi

# The weights ship as a PyTorch checkpoint, so a throwaway environment exports them to ONNX once.
[[ -x "$venv/bin/python" ]] || python3 -m venv "$venv"
"$venv/bin/pip" install --quiet --upgrade pip
"$venv/bin/pip" install --quiet torch torchaudio onnx 'openunmix==1.3.0'
"$venv/bin/python" - "$weights" "$file.partial" <<'PY'
import sys, warnings
warnings.filterwarnings("ignore")
import torch
from openunmix import utils
from openunmix.model import OpenUnmix
model = OpenUnmix(nb_bins=2049, nb_channels=2, hidden_size=1024, max_bin=utils.bandwidth_to_max_bin(44100.0, 4096, 16000))
model.load_state_dict(torch.load(sys.argv[1], map_location="cpu"), strict=False)
model.eval()
torch.onnx.export(model, torch.rand(1, 2, 2049, 32), sys.argv[2], input_names=["mag"], output_names=["estimate"], opset_version=17, dynamo=False)
PY
mv "$file.partial" "$file"
trap - EXIT
printf 'Installed clean Sing model: %s\n' "$file"
