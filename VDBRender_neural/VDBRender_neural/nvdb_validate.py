#!/usr/bin/env python3
"""
nvdb_validate.py — Validate neural VDB compression quality.

Compares original .vdb data against decoded .nvdb output to compute
quality metrics (PSNR, MSE, max error) and optionally renders side-by-side
comparison images.

Usage:
    python nvdb_validate.py --original smoke.vdb --compressed smoke.nvdb
    python nvdb_validate.py --original smoke.vdb --compressed smoke.nvdb --render --output comparison.exr
"""

import argparse
import struct
import sys
import os
import io
import json
import time

import numpy as np
import torch

try:
    import pyopenvdb as vdb
except ImportError:
    print("ERROR: pyopenvdb required. pip install pyopenvdb")
    sys.exit(1)


# ═══════════════════════════════════════════════════════════════════
# Network Definitions (must match nvdb_encoder.py)
# ═══════════════════════════════════════════════════════════════════

class PositionalEncoder(torch.nn.Module):
    def __init__(self, num_frequencies=6):
        super().__init__()
        self.num_frequencies = num_frequencies
        freqs = 2.0 ** torch.arange(num_frequencies).float()
        self.register_buffer("freqs", freqs)

    def forward(self, x):
        features = [x]
        for freq in self.freqs:
            features.append(torch.sin(freq * np.pi * x))
            features.append(torch.cos(freq * np.pi * x))
        return torch.cat(features, dim=-1)

    @property
    def output_dim(self):
        return 3 + 6 * self.num_frequencies


class TopologyClassifier(torch.nn.Module):
    def __init__(self, input_dim, hidden_dim=64, num_layers=3):
        super().__init__()
        layers = []
        layers.append(torch.nn.Linear(input_dim, hidden_dim))
        layers.append(torch.nn.ReLU(inplace=True))
        for _ in range(num_layers - 2):
            layers.append(torch.nn.Linear(hidden_dim, hidden_dim))
            layers.append(torch.nn.ReLU(inplace=True))
        layers.append(torch.nn.Linear(hidden_dim, 1))
        layers.append(torch.nn.Sigmoid())
        self.layers = torch.nn.Sequential(*layers)

    def forward(self, x):
        return self.layers(x).squeeze(-1)


class ValueRegressor(torch.nn.Module):
    def __init__(self, input_dim, hidden_dim=128, num_layers=4):
        super().__init__()
        layers = []
        layers.append(torch.nn.Linear(input_dim, hidden_dim))
        layers.append(torch.nn.ReLU(inplace=True))
        for _ in range(num_layers - 2):
            layers.append(torch.nn.Linear(hidden_dim, hidden_dim))
            layers.append(torch.nn.ReLU(inplace=True))
        layers.append(torch.nn.Linear(hidden_dim, 1))
        self.layers = torch.nn.Sequential(*layers)

    def forward(self, x):
        return self.layers(x).squeeze(-1)


# ═══════════════════════════════════════════════════════════════════
# NVDB Reader (Python side)
# ═══════════════════════════════════════════════════════════════════

NVDB_MAGIC = 0x4E564442

def read_nvdb_header(filepath):
    """Read and parse an NVDB file header."""
    with open(filepath, "rb") as f:
        data = f.read(128)

    fields = struct.unpack("<4I 3f 3i 3i QQf 2I I 4I 20s", data)

    return {
        "magic": fields[0],
        "version_major": fields[1],
        "version_minor": fields[2],
        "flags": fields[3],
        "voxel_size": fields[4:7],
        "bbox_min": fields[7:10],
        "bbox_max": fields[10:13],
        "original_bytes": fields[13],
        "compressed_bytes": fields[14],
        "psnr": fields[15],
        "grid_class": fields[16],
        "value_type": fields[17],
        "num_frequencies": fields[18],
        "topo_hidden": fields[19],
        "topo_layers": fields[20],
        "value_hidden": fields[21],
        "value_layers": fields[22],
    }


def read_nvdb_sections(filepath):
    """Read all sections from an NVDB file."""
    sections = {}

    with open(filepath, "rb") as f:
        f.read(128)  # Skip header

        while True:
            sh_data = f.read(16)
            if len(sh_data) < 16:
                break

            magic, sec_type, sec_size = struct.unpack("<IIQ", sh_data)
            if magic != NVDB_MAGIC:
                break

            if sec_type == 0xFF:  # END
                break

            data = f.read(sec_size)
            sections[sec_type] = data

    return sections


def load_nvdb_models(filepath, device="cpu"):
    """Load trained models from an NVDB file."""
    header = read_nvdb_header(filepath)
    sections = read_nvdb_sections(filepath)

    # Create encoder
    num_freq = header["num_frequencies"]
    encoder = PositionalEncoder(num_freq).to(device)
    input_dim = encoder.output_dim

    # Load topology model (section type 0x02)
    topo_data = sections.get(0x02, b"")
    if topo_data:
        topo_module = torch.jit.load(io.BytesIO(topo_data), map_location=device)
        topo_module.eval()
    else:
        topo_module = None

    # Load value model (section type 0x03)
    value_data = sections.get(0x03, b"")
    if value_data:
        value_module = torch.jit.load(io.BytesIO(value_data), map_location=device)
        value_module.eval()
    else:
        value_module = None

    return header, encoder, topo_module, value_module


# ═══════════════════════════════════════════════════════════════════
# Validation
# ═══════════════════════════════════════════════════════════════════

def validate(args):
    device = torch.device(args.device if args.device != "auto"
                          else ("cuda" if torch.cuda.is_available() else "cpu"))
    print(f"Device: {device}")

    # Load original VDB
    print(f"\nLoading original: {args.original}")
    grids = vdb.readAll(args.original)
    grid = None
    for g in grids:
        if isinstance(g, vdb.FloatGrid):
            grid = g
            break
    if grid is None:
        print("ERROR: No FloatGrid found in original VDB")
        return

    # Extract original data
    print("Extracting active voxels...")
    active_voxels = list(grid.citerOnValues())
    orig_coords = []
    orig_values = []
    for item in active_voxels:
        if not hasattr(item, "min") or hasattr(item, "count"):
            continue
        orig_coords.append(item.min)
        orig_values.append(item.value if isinstance(item.value, (int, float)) else 0.0)

    orig_coords = np.array(orig_coords, dtype=np.float32)
    orig_values = np.array(orig_values, dtype=np.float32)
    print(f"  Active voxels: {len(orig_values):,}")
    print(f"  Value range: [{orig_values.min():.4f}, {orig_values.max():.4f}]")

    # Load NVDB
    print(f"\nLoading compressed: {args.compressed}")
    header, encoder, topo_module, value_module = load_nvdb_models(args.compressed, device)

    print(f"  Version: {header['version_major']}.{header['version_minor']}")
    print(f"  Original: {header['original_bytes']:,} bytes")
    print(f"  Compressed: {header['compressed_bytes']:,} bytes")
    ratio = header['original_bytes'] / max(header['compressed_bytes'], 1)
    print(f"  Compression: {ratio:.1f}x")
    print(f"  Reported PSNR: {header['psnr']:.1f} dB")

    # Convert coordinates to world space and normalise
    xform = grid.transform
    vs = xform.voxelSize()
    voxel_size = np.array([vs[0], vs[1], vs[2]], dtype=np.float32)
    world_coords = orig_coords * voxel_size[None, :]

    bbox_min_w = world_coords.min(axis=0)
    bbox_max_w = world_coords.max(axis=0)
    center = (bbox_min_w + bbox_max_w) / 2.0
    extent = np.maximum((bbox_max_w - bbox_min_w) / 2.0, 1e-6)
    norm_coords = (world_coords - center) / extent

    # Normalise original values to [0, 1]
    v_min, v_max = orig_values.min(), orig_values.max()
    v_range = max(v_max - v_min, 1e-8)
    norm_values = (orig_values - v_min) / v_range

    # Decode through neural networks
    print("\nDecoding through neural networks...")
    coords_t = torch.from_numpy(norm_coords).to(device)

    with torch.no_grad():
        encoded = encoder(coords_t)

        # Topology
        if topo_module is not None:
            topo_pred = topo_module(encoded).cpu().numpy()
        else:
            topo_pred = np.ones(len(norm_values))

        # Values (for active voxels)
        active_mask = topo_pred > 0.5
        decoded_values = np.zeros(len(norm_values), dtype=np.float32)

        if value_module is not None and active_mask.sum() > 0:
            active_idx = np.where(active_mask)[0]
            active_encoded = encoded[torch.from_numpy(active_idx).long()]

            val_pred = value_module(active_encoded).cpu().numpy()
            val_pred = np.clip(val_pred, 0.0, 1.0)
            decoded_values[active_idx] = val_pred

    # Compute metrics
    print("\n═══════════════════════════════════════")
    print("QUALITY METRICS")
    print("═══════════════════════════════════════")

    # Topology accuracy
    gt_active = norm_values > 0.001
    topo_correct = (active_mask == gt_active).sum()
    topo_accuracy = topo_correct / len(gt_active) * 100
    false_pos = ((active_mask == True) & (gt_active == False)).sum()
    false_neg = ((active_mask == False) & (gt_active == True)).sum()
    print(f"\nTopology:")
    print(f"  Accuracy:      {topo_accuracy:.2f}%")
    print(f"  False positive: {false_pos:,} ({false_pos/len(gt_active)*100:.2f}%)")
    print(f"  False negative: {false_neg:,} ({false_neg/len(gt_active)*100:.2f}%)")

    # Value reconstruction quality
    mse = np.mean((decoded_values - norm_values) ** 2)
    rmse = np.sqrt(mse)
    psnr = -10 * np.log10(max(mse, 1e-10))
    max_error = np.max(np.abs(decoded_values - norm_values))

    # Only on active voxels
    active_gt = norm_values[gt_active]
    active_decoded = decoded_values[gt_active]
    if len(active_gt) > 0:
        active_mse = np.mean((active_decoded - active_gt) ** 2)
        active_psnr = -10 * np.log10(max(active_mse, 1e-10))
    else:
        active_psnr = float('inf')

    print(f"\nValue reconstruction (all voxels):")
    print(f"  MSE:           {mse:.6f}")
    print(f"  RMSE:          {rmse:.6f}")
    print(f"  PSNR:          {psnr:.1f} dB")
    print(f"  Max error:     {max_error:.6f}")

    print(f"\nValue reconstruction (active voxels only):")
    print(f"  PSNR:          {active_psnr:.1f} dB")

    # Compression
    print(f"\nCompression:")
    print(f"  Ratio:         {ratio:.1f}x")
    print(f"  Original:      {header['original_bytes']/1024/1024:.1f} MB")
    print(f"  Compressed:    {header['compressed_bytes']/1024/1024:.1f} MB")

    # Error histogram
    errors = np.abs(decoded_values - norm_values)
    percentiles = [50, 90, 95, 99, 99.9]
    print(f"\nError distribution:")
    for p in percentiles:
        val = np.percentile(errors, p)
        print(f"  P{p:5.1f}:        {val:.6f}")

    print("\n═══════════════════════════════════════")

    return psnr


def main():
    parser = argparse.ArgumentParser(description="NeuralVDB Validation")
    parser.add_argument("--original", "-o", required=True, help="Original .vdb file")
    parser.add_argument("--compressed", "-c", required=True, help="Compressed .nvdb file")
    parser.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"])

    args = parser.parse_args()
    validate(args)


if __name__ == "__main__":
    main()
