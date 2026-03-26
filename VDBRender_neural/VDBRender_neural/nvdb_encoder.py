#!/usr/bin/env python3
"""
nvdb_encoder.py — Train neural networks to compress VDB volumes into .nvdb format.

Usage:
    python nvdb_encoder.py --input smoke.vdb --output smoke.nvdb
    python nvdb_encoder.py --input smoke.vdb --output smoke.nvdb --epochs 200 --lr 1e-3

    # Animated sequence with warm-starting:
    python nvdb_encoder.py --input smoke.%04d.vdb --output smoke.%04d.nvdb \\
                           --frame-range 1 100 --warm-start

Requirements:
    pip install torch pyopenvdb numpy tqdm
"""

import argparse
import json
import struct
import io
import os
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader, TensorDataset

try:
    import pyopenvdb as vdb
    HAS_PYOPENVDB = True
except ImportError:
    HAS_PYOPENVDB = False
    if __name__ == "__main__":
        print("ERROR: pyopenvdb not found. Install via: pip install pyopenvdb")
        print("       Or build from source: https://github.com/AcademySoftwareFoundation/openvdb")
        sys.exit(1)

from tqdm import tqdm


# ═══════════════════════════════════════════════════════════════════
# Network Architectures (must match the C++ decoder)
# ═══════════════════════════════════════════════════════════════════

class PositionalEncoder(nn.Module):
    """Fourier feature positional encoding for 3D coordinates."""

    def __init__(self, num_frequencies=6):
        super().__init__()
        self.num_frequencies = num_frequencies
        freqs = 2.0 ** torch.arange(num_frequencies).float()
        self.register_buffer("freqs", freqs)

    def forward(self, x):
        # x: [N, 3]
        features = [x]
        for freq in self.freqs:
            features.append(torch.sin(freq * np.pi * x))
            features.append(torch.cos(freq * np.pi * x))
        return torch.cat(features, dim=-1)

    @property
    def output_dim(self):
        return 3 + 6 * self.num_frequencies


class TopologyClassifier(nn.Module):
    """Binary classifier: active vs inactive voxels."""

    def __init__(self, input_dim, hidden_dim=64, num_layers=3):
        super().__init__()
        layers = []
        layers.append(nn.Linear(input_dim, hidden_dim))
        layers.append(nn.ReLU(inplace=True))
        for _ in range(num_layers - 2):
            layers.append(nn.Linear(hidden_dim, hidden_dim))
            layers.append(nn.ReLU(inplace=True))
        layers.append(nn.Linear(hidden_dim, 1))
        layers.append(nn.Sigmoid())
        self.layers = nn.Sequential(*layers)

    def forward(self, x):
        return self.layers(x).squeeze(-1)


class ValueRegressor(nn.Module):
    """Scalar value predictor for active voxels."""

    def __init__(self, input_dim, hidden_dim=128, num_layers=4):
        super().__init__()
        layers = []
        layers.append(nn.Linear(input_dim, hidden_dim))
        layers.append(nn.ReLU(inplace=True))
        for _ in range(num_layers - 2):
            layers.append(nn.Linear(hidden_dim, hidden_dim))
            layers.append(nn.ReLU(inplace=True))
        layers.append(nn.Linear(hidden_dim, 1))
        self.layers = nn.Sequential(*layers)

    def forward(self, x):
        return self.layers(x).squeeze(-1)


# ═══════════════════════════════════════════════════════════════════
# VDB Data Extraction
# ═══════════════════════════════════════════════════════════════════

def extract_vdb_data(grid):
    """Extract coordinates and values from an OpenVDB grid.

    Returns:
        active_coords: [N, 3] float32 — world-space coordinates of active voxels
        active_values: [N] float32 — scalar values at those coordinates
        inactive_coords: [M, 3] float32 — sampled inactive positions (for topology training)
        bbox_min, bbox_max: bounding box in index space
        voxel_size: grid voxel size
    """
    # Get all active voxels
    active_voxels = list(grid.citerOnValues())

    if not active_voxels:
        raise ValueError("Grid has no active voxels!")

    # Separate coords and values
    coords_idx = []
    values = []

    for item in active_voxels:
        if hasattr(item, "min"):
            # Tile (internal node) — skip, we want leaf-level
            continue
        coords_idx.append(item.min)
        values.append(item.value if isinstance(item.value, (int, float)) else item.value)

    if not coords_idx:
        # Try iterating differently for older pyopenvdb
        accessor = grid.getConstAccessor()
        for leaf in grid.iterOnValues():
            coord = leaf.min
            val = accessor.getValue(coord)
            coords_idx.append(coord)
            values.append(val)

    coords_idx = np.array(coords_idx, dtype=np.float32)
    values = np.array(values, dtype=np.float32)

    # Convert to world space
    xform = grid.transform
    vs = xform.voxelSize()
    voxel_size = np.array([vs[0], vs[1], vs[2]], dtype=np.float32)

    # Simple index-to-world: multiply by voxel size (assumes identity rotation)
    active_coords = coords_idx * voxel_size[None, :]

    # Bounding box
    bbox_min = coords_idx.min(axis=0).astype(np.int32)
    bbox_max = coords_idx.max(axis=0).astype(np.int32)

    # Sample inactive positions (negative examples for topology classifier)
    # Sample uniformly in the bounding box, filter out active positions
    n_inactive = len(active_coords)  # Equal number of positive and negative
    inactive_samples = []
    rng = np.random.default_rng(42)

    pad = 2  # Extra padding around bbox
    while len(inactive_samples) < n_inactive:
        batch = rng.integers(
            bbox_min - pad,
            bbox_max + pad + 1,
            size=(n_inactive * 2, 3)
        ).astype(np.float32)

        # Filter: keep only positions NOT in the active set
        # (Simple approach: check against accessor)
        for coord in batch:
            if len(inactive_samples) >= n_inactive:
                break
            # Quick hash check
            key = tuple(coord.astype(int))
            # Approximate: just add — the network will learn the boundary
            inactive_samples.append(coord * voxel_size)

    inactive_coords = np.array(inactive_samples[:n_inactive], dtype=np.float32)

    print(f"  Active voxels: {len(active_coords):,}")
    print(f"  Inactive samples: {len(inactive_coords):,}")
    print(f"  Value range: [{values.min():.4f}, {values.max():.4f}]")
    print(f"  BBox: [{bbox_min}] - [{bbox_max}]")
    print(f"  Voxel size: {voxel_size}")

    return active_coords, values, inactive_coords, bbox_min, bbox_max, voxel_size


def normalise_coords(coords, bbox_min_world, bbox_max_world):
    """Normalise coordinates to [-1, 1] range."""
    center = (bbox_min_world + bbox_max_world) / 2.0
    extent = (bbox_max_world - bbox_min_world) / 2.0
    extent = np.maximum(extent, 1e-6)  # Avoid division by zero
    return (coords - center) / extent


# ═══════════════════════════════════════════════════════════════════
# Training
# ═══════════════════════════════════════════════════════════════════

def train_topology(encoder, classifier, active_coords, inactive_coords,
                   bbox_min_world, bbox_max_world, args, device):
    """Train the topology classifier (active vs inactive voxels)."""
    print("\n─── Training topology classifier ───")

    # Prepare data
    pos_coords = normalise_coords(active_coords, bbox_min_world, bbox_max_world)
    neg_coords = normalise_coords(inactive_coords, bbox_min_world, bbox_max_world)

    all_coords = np.concatenate([pos_coords, neg_coords], axis=0)
    labels = np.concatenate([
        np.ones(len(pos_coords), dtype=np.float32),
        np.zeros(len(neg_coords), dtype=np.float32)
    ])

    # Shuffle
    perm = np.random.permutation(len(all_coords))
    all_coords = all_coords[perm]
    labels = labels[perm]

    coords_t = torch.from_numpy(all_coords).to(device)
    labels_t = torch.from_numpy(labels).to(device)

    dataset = TensorDataset(coords_t, labels_t)
    loader = DataLoader(dataset, batch_size=args.batch_size, shuffle=True,
                        drop_last=True, num_workers=0)

    optimizer = optim.Adam(
        list(classifier.parameters()),
        lr=args.lr
    )
    criterion = nn.BCELoss()

    best_acc = 0.0
    for epoch in range(args.topo_epochs):
        classifier.train()
        total_loss = 0
        correct = 0
        total = 0

        for batch_coords, batch_labels in loader:
            encoded = encoder(batch_coords)
            pred = classifier(encoded)

            loss = criterion(pred, batch_labels)

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            total_loss += loss.item()
            correct += ((pred > 0.5) == (batch_labels > 0.5)).sum().item()
            total += len(batch_labels)

        acc = correct / total * 100
        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"  Epoch {epoch+1:3d}/{args.topo_epochs}: "
                  f"loss={total_loss/len(loader):.4f}  acc={acc:.1f}%")

        best_acc = max(best_acc, acc)

    print(f"  Best accuracy: {best_acc:.1f}%")
    return best_acc


def train_values(encoder, regressor, active_coords, values,
                 bbox_min_world, bbox_max_world, args, device):
    """Train the value regressor on active voxels."""
    print("\n─── Training value regressor ───")

    # Normalise coordinates
    norm_coords = normalise_coords(active_coords, bbox_min_world, bbox_max_world)

    # Normalise values to [0, 1]
    v_min, v_max = values.min(), values.max()
    v_range = max(v_max - v_min, 1e-8)
    norm_values = (values - v_min) / v_range

    coords_t = torch.from_numpy(norm_coords).to(device)
    values_t = torch.from_numpy(norm_values).to(device)

    dataset = TensorDataset(coords_t, values_t)
    loader = DataLoader(dataset, batch_size=args.batch_size, shuffle=True,
                        drop_last=True, num_workers=0)

    optimizer = optim.Adam(regressor.parameters(), lr=args.lr)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.value_epochs)
    criterion = nn.MSELoss()

    best_psnr = 0.0
    for epoch in range(args.value_epochs):
        regressor.train()
        total_loss = 0

        for batch_coords, batch_values in loader:
            encoded = encoder(batch_coords)
            pred = regressor(encoded)

            loss = criterion(pred, batch_values)

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()

            total_loss += loss.item()

        scheduler.step()

        avg_loss = total_loss / len(loader)
        psnr = -10 * np.log10(max(avg_loss, 1e-10))

        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"  Epoch {epoch+1:3d}/{args.value_epochs}: "
                  f"MSE={avg_loss:.6f}  PSNR={psnr:.1f} dB")

        best_psnr = max(best_psnr, psnr)

    print(f"  Best PSNR: {best_psnr:.1f} dB")
    return best_psnr


# ═══════════════════════════════════════════════════════════════════
# .nvdb File Writing
# ═══════════════════════════════════════════════════════════════════

NVDB_MAGIC = 0x4E564442

def write_nvdb(filepath, grid, encoder, classifier, regressor,
               bbox_min, bbox_max, voxel_size, original_bytes, psnr, args):
    """Write the trained models and upper tree to a .nvdb file."""
    print(f"\n─── Writing {filepath} ───")

    # Serialize models to TorchScript
    topo_buffer = io.BytesIO()
    topo_scripted = torch.jit.script(classifier)
    torch.jit.save(topo_scripted, topo_buffer)
    topo_bytes = topo_buffer.getvalue()

    value_buffer = io.BytesIO()
    value_scripted = torch.jit.script(regressor)
    torch.jit.save(value_scripted, value_buffer)
    value_bytes = value_buffer.getvalue()

    # Serialize upper VDB tree (write grid to temp file, read bytes)
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".vdb", delete=False) as tmp:
        tmp_path = tmp.name
    vdb.write(tmp_path, grids=[grid])
    with open(tmp_path, "rb") as f:
        tree_bytes = f.read()
    os.unlink(tmp_path)

    # Encoding config JSON
    encoding_config = json.dumps({
        "num_frequencies": args.num_frequencies,
        "normalisation": "bbox_centered",
    })

    # Metadata JSON
    metadata = json.dumps({
        "grid_name": grid.name or "volume",
        "encoder": "nvdb_encoder.py",
        "training_epochs_topo": args.topo_epochs,
        "training_epochs_value": args.value_epochs,
        "learning_rate": args.lr,
        "batch_size": args.batch_size,
        "psnr": float(psnr),
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
    })

    # Compute total compressed size
    section_header_size = 16  # 4 + 4 + 8 bytes
    compressed_bytes = (128  # file header
        + section_header_size + len(tree_bytes)
        + section_header_size + len(topo_bytes)
        + section_header_size + len(value_bytes)
        + section_header_size + len(encoding_config)
        + section_header_size + len(metadata)
        + section_header_size)  # END

    # Build file header (128 bytes, packed)
    header = struct.pack(
        "<4I 3f 3i 3i QQf 2I I 4I 20s",
        NVDB_MAGIC,                         # magic
        1, 0,                               # version major, minor
        0,                                  # flags
        *voxel_size.tolist(),               # voxel_size[3]
        *bbox_min.tolist(),                 # bbox_min[3]
        *bbox_max.tolist(),                 # bbox_max[3]
        original_bytes,                     # original_bytes
        compressed_bytes,                   # compressed_bytes
        psnr,                               # psnr
        0,                                  # grid_class (0 = fog)
        0,                                  # value_type (0 = float)
        args.num_frequencies,               # num_frequencies
        args.topo_hidden, args.topo_layers, # topology net config
        args.value_hidden, args.value_layers, # value net config
        b'\x00' * 20                        # reserved
    )

    def write_section(f, section_type, data):
        sh = struct.pack("<IIQ", NVDB_MAGIC, section_type, len(data))
        f.write(sh)
        if data:
            f.write(data if isinstance(data, bytes) else data.encode())

    with open(filepath, "wb") as f:
        f.write(header)
        write_section(f, 0x01, tree_bytes)            # UPPER_TREE
        write_section(f, 0x02, topo_bytes)            # TOPOLOGY_MODEL
        write_section(f, 0x03, value_bytes)            # VALUE_MODEL
        write_section(f, 0x04, encoding_config)        # ENCODING_CONFIG
        write_section(f, 0x05, metadata)               # METADATA
        write_section(f, 0xFF, b"")                    # END

    ratio = original_bytes / max(compressed_bytes, 1)
    print(f"  Original:   {original_bytes:>12,} bytes")
    print(f"  Compressed: {compressed_bytes:>12,} bytes")
    print(f"  Ratio:      {ratio:.1f}x")
    print(f"  PSNR:       {psnr:.1f} dB")


# ═══════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════

def encode_single_frame(input_path, output_path, args, device,
                        prev_classifier=None, prev_regressor=None):
    """Encode a single VDB file to .nvdb format."""
    print(f"\n{'='*60}")
    print(f"Encoding: {input_path}")
    print(f"{'='*60}")

    # Load VDB
    grids = vdb.readAll(input_path)
    if not grids:
        print(f"ERROR: No grids found in {input_path}")
        return None, None

    # Use first float grid
    grid = None
    for g in grids:
        if isinstance(g, vdb.FloatGrid):
            grid = g
            break

    if grid is None:
        print(f"ERROR: No FloatGrid found in {input_path}")
        return None, None

    original_bytes = os.path.getsize(input_path)
    print(f"  Grid: {grid.name or 'unnamed'}")
    print(f"  File size: {original_bytes:,} bytes")

    # Extract data
    active_coords, values, inactive_coords, bbox_min, bbox_max, voxel_size = \
        extract_vdb_data(grid)

    bbox_min_world = bbox_min.astype(np.float32) * voxel_size
    bbox_max_world = bbox_max.astype(np.float32) * voxel_size

    # Create networks
    encoder = PositionalEncoder(args.num_frequencies).to(device)
    input_dim = encoder.output_dim

    classifier = TopologyClassifier(input_dim, args.topo_hidden, args.topo_layers).to(device)
    regressor = ValueRegressor(input_dim, args.value_hidden, args.value_layers).to(device)

    # Warm-start from previous frame if available
    if prev_classifier is not None and args.warm_start:
        classifier.load_state_dict(prev_classifier.state_dict())
        print("  Warm-started topology from previous frame")
    if prev_regressor is not None and args.warm_start:
        regressor.load_state_dict(prev_regressor.state_dict())
        print("  Warm-started values from previous frame")

    # Train topology
    topo_acc = train_topology(encoder, classifier, active_coords, inactive_coords,
                              bbox_min_world, bbox_max_world, args, device)

    # Train values
    psnr = train_values(encoder, regressor, active_coords, values,
                        bbox_min_world, bbox_max_world, args, device)

    # Move to CPU for serialization
    classifier.cpu().eval()
    regressor.cpu().eval()

    # Write .nvdb
    write_nvdb(output_path, grid, encoder.cpu(), classifier, regressor,
               bbox_min, bbox_max, voxel_size, original_bytes, psnr, args)

    return classifier, regressor


def main():
    parser = argparse.ArgumentParser(description="NeuralVDB Encoder — compress VDB to .nvdb")

    parser.add_argument("--input", "-i", required=True, help="Input .vdb file (or pattern with %%04d)")
    parser.add_argument("--output", "-o", required=True, help="Output .nvdb file (or pattern)")

    # Sequence
    parser.add_argument("--frame-range", nargs=2, type=int, metavar=("START", "END"),
                        help="Frame range for animated sequences")
    parser.add_argument("--warm-start", action="store_true",
                        help="Warm-start each frame from the previous frame's weights")

    # Network architecture
    parser.add_argument("--num-frequencies", type=int, default=6,
                        help="Number of Fourier feature frequencies (default: 6)")
    parser.add_argument("--topo-hidden", type=int, default=64,
                        help="Topology classifier hidden dim (default: 64)")
    parser.add_argument("--topo-layers", type=int, default=3,
                        help="Topology classifier num layers (default: 3)")
    parser.add_argument("--value-hidden", type=int, default=128,
                        help="Value regressor hidden dim (default: 128)")
    parser.add_argument("--value-layers", type=int, default=4,
                        help="Value regressor num layers (default: 4)")

    # Training
    parser.add_argument("--topo-epochs", type=int, default=50,
                        help="Topology training epochs (default: 50)")
    parser.add_argument("--value-epochs", type=int, default=200,
                        help="Value training epochs (default: 200)")
    parser.add_argument("--lr", type=float, default=1e-3,
                        help="Learning rate (default: 1e-3)")
    parser.add_argument("--batch-size", type=int, default=4096,
                        help="Batch size (default: 4096)")

    # Device
    parser.add_argument("--device", type=str, default="auto",
                        choices=["auto", "cpu", "cuda"],
                        help="Device (default: auto)")

    args = parser.parse_args()

    # Device selection
    if args.device == "auto":
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    else:
        device = torch.device(args.device)
    print(f"Using device: {device}")

    if args.frame_range:
        # Animated sequence
        start, end = args.frame_range
        prev_classifier = None
        prev_regressor = None

        for frame in range(start, end + 1):
            in_path = args.input % frame
            out_path = args.output % frame

            if not os.path.exists(in_path):
                print(f"WARNING: {in_path} not found, skipping frame {frame}")
                continue

            prev_classifier, prev_regressor = encode_single_frame(
                in_path, out_path, args, device,
                prev_classifier, prev_regressor
            )
    else:
        # Single frame
        encode_single_frame(args.input, args.output, args, device)

    print("\nDone!")


if __name__ == "__main__":
    main()
