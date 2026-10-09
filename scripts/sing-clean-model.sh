#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

file='.cache/models/umxl-vocals.onnx'
weights='.cache/models/umxl-vocals.pth'
url='https://zenodo.org/records/5069601/files/vocals-bccbd9aa.pth'
sha='bccbd9aa5414a5a1c869277dd9e44f1522beca0b2b2639a84fe4eeb911382fe6'
venv='.cache/sing-export'
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

# The weights ship as a PyTorch checkpoint; this reads it directly and writes the ONNX graph, so PyTorch is not needed.
[[ -x "$venv/bin/python" ]] || python3 -m venv "$venv"
"$venv/bin/python" -m pip install --quiet --disable-pip-version-check --timeout 60 numpy onnx
"$venv/bin/python" - "$weights" "$file.partial" <<'PY'
import collections, pickle, sys, zipfile
import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

KINDS = {"FloatStorage": np.float32, "DoubleStorage": np.float64, "HalfStorage": np.float16, "LongStorage": np.int64, "IntStorage": np.int32}

class Lazy:
    def __init__(self, storage, offset, size, stride): self.storage, self.offset, self.size, self.stride = storage, offset, tuple(size), tuple(stride)
    def array(self):
        data = self.storage.data
        if not self.size: return data[self.offset:self.offset + 1].reshape(()).copy()
        return np.lib.stride_tricks.as_strided(data[self.offset:], self.size, tuple(s * data.itemsize for s in self.stride)).copy()

class Storage:
    def __init__(self, kind): self.kind, self.data = kind, None

def read(stream, storages):
    class Reader(pickle.Unpickler):
        def find_class(self, module, name):
            if module == "torch._utils" and name == "_rebuild_tensor_v2": return lambda st, off, size, stride, *rest: Lazy(st, off, size, stride)
            if module == "torch" and name in KINDS: return KINDS[name]
            if module == "collections" and name == "OrderedDict": return collections.OrderedDict
            raise pickle.UnpicklingError(f"unexpected {module}.{name}")
        def persistent_load(self, pid):
            kind, key = pid[1], pid[2]
            return storages.setdefault(key, Storage(kind))
    return Reader(stream).load()

def load(path):
    storages = {}
    if zipfile.is_zipfile(path):
        archive = zipfile.ZipFile(path); root = archive.namelist()[0].split("/")[0]
        state = read(archive.open(f"{root}/data.pkl"), storages)
        for key, st in storages.items(): st.data = np.frombuffer(archive.read(f"{root}/data/{key}"), st.kind)
    else:
        # Legacy format: magic, version and system info pickles, the state dict, the storage keys, then raw storages.
        with open(path, "rb") as f:
            for _ in range(3): pickle.load(f)
            state = read(f, storages)
            for key in pickle.load(f):
                st = storages[key]; count = int(np.frombuffer(f.read(8), "<i8")[0])
                st.data = np.frombuffer(f.read(count * np.dtype(st.kind).itemsize), st.kind)
    return {k: v.array() if isinstance(v, Lazy) else v for k, v in state.items()}

def build(weights, frames, out):
    w = {k: np.asarray(v, np.float32) for k, v in weights.items() if not k.endswith("num_batches_tracked")}
    used = w["input_mean"].shape[0]; hidden = w["fc1.weight"].shape[0]; bins = w["output_mean"].shape[0]; half = hidden // 2
    inits, nodes = [], []
    def const(name, value):
        inits.append(numpy_helper.from_array(np.asarray(value), name)); return name
    def node(op, inputs, **attrs):
        name = f"n{len(nodes)}"; nodes.append(helper.make_node(op, inputs, [name], **attrs)); return name
    def norm(x, prefix):
        scale = w[f"{prefix}.weight"] / np.sqrt(w[f"{prefix}.running_var"] + 1e-5)
        return node("Add", [node("Mul", [x, const(f"{prefix}_s", scale)]), const(f"{prefix}_b", w[f"{prefix}.bias"] - w[f"{prefix}.running_mean"] * scale)])
    mix = node("Transpose", ["mag"], perm=[3, 0, 1, 2])
    x = node("Slice", [mix, const("lo", np.array([0], np.int64)), const("hi", np.array([used], np.int64)), const("ax", np.array([3], np.int64))])
    x = node("Mul", [node("Add", [x, const("in_mean", w["input_mean"])]), const("in_scale", w["input_scale"])])
    x = node("Reshape", [x, const("flat", np.array([frames, 2 * used], np.int64))])
    x = node("Tanh", [norm(node("MatMul", [x, const("fc1", w["fc1.weight"].T)]), "bn1")])
    first = x
    y = node("Reshape", [x, const("seq", np.array([frames, 1, hidden], np.int64))])
    layer = 0
    while f"lstm.weight_ih_l{layer}" in w:
        def gates(m): i, f, g, o = np.split(m, 4); return np.concatenate([i, o, f, g])
        W = np.stack([gates(w[f"lstm.weight_ih_l{layer}{s}"]) for s in ("", "_reverse")])
        R = np.stack([gates(w[f"lstm.weight_hh_l{layer}{s}"]) for s in ("", "_reverse")])
        B = np.stack([np.concatenate([gates(w[f"lstm.bias_ih_l{layer}{s}"]), gates(w[f"lstm.bias_hh_l{layer}{s}"])]) for s in ("", "_reverse")])
        y = node("LSTM", [y, const(f"W{layer}", W), const(f"R{layer}", R), const(f"B{layer}", B)], hidden_size=half, direction="bidirectional")
        y = node("Reshape", [node("Transpose", [y], perm=[0, 2, 1, 3]), const(f"seq{layer}", np.array([frames, 1, hidden], np.int64))])
        layer += 1
    x = node("Concat", [first, node("Reshape", [y, const("flat2", np.array([frames, hidden], np.int64))])], axis=1)
    x = node("Relu", [norm(node("MatMul", [x, const("fc2", w["fc2.weight"].T)]), "bn2")])
    x = norm(node("MatMul", [x, const("fc3", w["fc3.weight"].T)]), "bn3")
    x = node("Reshape", [x, const("shape", np.array([frames, 1, 2, bins], np.int64))])
    x = node("Relu", [node("Add", [node("Mul", [x, const("out_scale", w["output_scale"])]), const("out_mean", w["output_mean"])])])
    nodes.append(helper.make_node("Transpose", [node("Mul", [x, mix])], ["estimate"], perm=[1, 2, 3, 0]))
    graph = helper.make_graph(nodes, "umx", [helper.make_tensor_value_info("mag", TensorProto.FLOAT, [1, 2, bins, frames])],
                              [helper.make_tensor_value_info("estimate", TensorProto.FLOAT, [1, 2, bins, frames])], inits)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    onnx.save(model, out)

build(load(sys.argv[1]), 32, sys.argv[2])
PY
mv "$file.partial" "$file"
trap - EXIT
printf 'Installed clean Sing model: %s\n' "$file"
