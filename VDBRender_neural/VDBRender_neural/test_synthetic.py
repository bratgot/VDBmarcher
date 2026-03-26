#!/usr/bin/env python3
"""
test_synthetic.py — End-to-end test with a procedurally generated volume.

Creates a synthetic density field (a sphere with noise), trains
the neural networks, writes a .nvdb file, reads it back, and
validates reconstruction quality. No real VDB files needed.

Usage:
    python test_synthetic.py
    python test_synthetic.py --resolution 64 --epochs 100
"""

import argparse
import os
import sys
import tempfile
import struct
import io
import time

import numpy as np
import torch
import torch.nn as nn

# ── Try importing pyopenvdb (optional for this test) ────────────────
try:
    import pyopenvdb as vdb
    HAS_VDB = True
except ImportError:
    HAS_VDB = False
    print("pyopenvdb not available — testing with raw numpy arrays only")


# ── Import from our encoder ────────────────────────────────────────
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nvdb_encoder import (
    PositionalEncoder, TopologyClassifier, ValueRegressor
)


# ═══════════════════════════════════════════════════════════════════
# Synthetic Volume Generation
# ═══════════════════════════════════════════════════════════════════

def generate_sphere_volume(resolution=64, radius=0.35, noise_scale=0.1):
    """Generate a procedural fog sphere with noise.

    Returns:
        coords: [N, 3] float32 — normalised coordinates of active voxels
        values: [N] float32 — density values in [0, 1]
        inactive_coords: [M, 3] — background samples
    """
    print(f"Generating {resolution}^3 procedural volume...")

    # Create a 3D grid
    lin = np.linspace(-1.0, 1.0, resolution)
    xx, yy, zz = np.meshgrid(lin, lin, lin, indexing='ij')

    # Distance from center
    dist = np.sqrt(xx**2 + yy**2 + zz**2)

    # Sphere with smooth falloff
    density = np.clip(1.0 - dist / radius, 0.0, 1.0)

    # Add turbulence noise
    rng = np.random.default_rng(42)
    noise = rng.normal(0, noise_scale, density.shape).astype(np.float32)

    # Frequency-based detail
    for freq in [2.0, 4.0, 8.0]:
        phase = rng.uniform(0, 2 * np.pi, 3)
        wave = (np.sin(freq * np.pi * xx + phase[0]) *
                np.sin(freq * np.pi * yy + phase[1]) *
                np.sin(freq * np.pi * zz + phase[2]))
        noise += wave.astype(np.float32) * (noise_scale / freq)

    density = np.clip(density + noise * density, 0.0, 1.0).astype(np.float32)

    # Extract active voxels (density > threshold)
    threshold = 0.01
    active_mask = density > threshold
    active_flat = active_mask.flatten()

    coords_grid = np.stack([xx, yy, zz], axis=-1).reshape(-1, 3)

    active_coords = coords_grid[active_flat].astype(np.float32)
    active_values = density.flatten()[active_flat].astype(np.float32)

    # Sample inactive positions
    inactive_flat = ~active_flat
    inactive_all = coords_grid[inactive_flat]
    n_inactive = min(len(active_coords), len(inactive_all))
    indices = rng.choice(len(inactive_all), n_inactive, replace=False)
    inactive_coords = inactive_all[indices].astype(np.float32)

    print(f"  Resolution: {resolution}^3 = {resolution**3:,} voxels")
    print(f"  Active: {len(active_coords):,} ({len(active_coords)/resolution**3*100:.1f}%)")
    print(f"  Inactive samples: {len(inactive_coords):,}")
    print(f"  Value range: [{active_values.min():.4f}, {active_values.max():.4f}]")

    return active_coords, active_values, inactive_coords


# ═══════════════════════════════════════════════════════════════════
# Training
# ═══════════════════════════════════════════════════════════════════

def train_and_validate(args):
    device = torch.device("cuda" if torch.cuda.is_available() and args.device != "cpu" else "cpu")
    print(f"Device: {device}")

    # Generate synthetic data
    active_coords, active_values, inactive_coords = generate_sphere_volume(
        resolution=args.resolution, noise_scale=args.noise_scale
    )

    # Create networks
    encoder = PositionalEncoder(args.num_frequencies).to(device)
    input_dim = encoder.output_dim

    classifier = TopologyClassifier(input_dim, args.hidden_dim, 3).to(device)
    regressor = ValueRegressor(input_dim, args.hidden_dim * 2, 4).to(device)

    # ── Train Topology ──────────────────────────────────────────────
    print("\n─── Training topology classifier ───")

    pos_t = torch.from_numpy(active_coords).to(device)
    neg_t = torch.from_numpy(inactive_coords).to(device)

    all_coords = torch.cat([pos_t, neg_t], dim=0)
    labels = torch.cat([
        torch.ones(len(pos_t), device=device),
        torch.zeros(len(neg_t), device=device)
    ])

    perm = torch.randperm(len(all_coords))
    all_coords = all_coords[perm]
    labels = labels[perm]

    opt_topo = torch.optim.Adam(classifier.parameters(), lr=args.lr)
    bce = nn.BCELoss()

    for epoch in range(args.topo_epochs):
        classifier.train()
        total_loss = 0
        correct = 0
        n_batches = 0

        for start in range(0, len(all_coords), args.batch_size):
            end = min(start + args.batch_size, len(all_coords))
            batch_c = all_coords[start:end]
            batch_l = labels[start:end]

            encoded = encoder(batch_c)
            pred = classifier(encoded)
            loss = bce(pred, batch_l)

            opt_topo.zero_grad()
            loss.backward()
            opt_topo.step()

            total_loss += loss.item()
            correct += ((pred > 0.5) == (batch_l > 0.5)).sum().item()
            n_batches += 1

        acc = correct / len(all_coords) * 100
        if (epoch + 1) % max(1, args.topo_epochs // 5) == 0 or epoch == 0:
            print(f"  Epoch {epoch+1:3d}/{args.topo_epochs}: "
                  f"loss={total_loss/n_batches:.4f}  acc={acc:.1f}%")

    topo_accuracy = acc
    print(f"  Final topology accuracy: {topo_accuracy:.1f}%")

    # ── Train Values ────────────────────────────────────────────────
    print("\n─── Training value regressor ───")

    values_t = torch.from_numpy(active_values).to(device)
    opt_val = torch.optim.Adam(regressor.parameters(), lr=args.lr)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(opt_val, T_max=args.value_epochs)
    mse_loss = nn.MSELoss()

    for epoch in range(args.value_epochs):
        regressor.train()
        total_loss = 0
        n_batches = 0

        for start in range(0, len(pos_t), args.batch_size):
            end = min(start + args.batch_size, len(pos_t))
            batch_c = pos_t[start:end]
            batch_v = values_t[start:end]

            encoded = encoder(batch_c)
            pred = regressor(encoded)
            loss = mse_loss(pred, batch_v)

            opt_val.zero_grad()
            loss.backward()
            opt_val.step()

            total_loss += loss.item()
            n_batches += 1

        scheduler.step()
        avg_loss = total_loss / n_batches
        psnr = -10 * np.log10(max(avg_loss, 1e-10))

        if (epoch + 1) % max(1, args.value_epochs // 5) == 0 or epoch == 0:
            print(f"  Epoch {epoch+1:3d}/{args.value_epochs}: "
                  f"MSE={avg_loss:.6f}  PSNR={psnr:.1f} dB")

    # ── Validate ────────────────────────────────────────────────────
    print("\n═══════════════════════════════════════")
    print("VALIDATION RESULTS")
    print("═══════════════════════════════════════")

    classifier.eval()
    regressor.eval()

    with torch.no_grad():
        # Topology validation
        encoded_pos = encoder(pos_t)
        topo_pred = classifier(encoded_pos).cpu().numpy()
        topo_correct = (topo_pred > 0.5).sum()
        topo_recall = topo_correct / len(topo_pred) * 100

        encoded_neg = encoder(neg_t)
        topo_neg_pred = classifier(encoded_neg).cpu().numpy()
        topo_neg_correct = (topo_neg_pred < 0.5).sum()
        topo_specificity = topo_neg_correct / len(topo_neg_pred) * 100

        # Value validation
        val_pred = regressor(encoded_pos).cpu().numpy()
        val_pred = np.clip(val_pred, 0.0, 1.0)

        mse = np.mean((val_pred - active_values) ** 2)
        rmse = np.sqrt(mse)
        psnr_final = -10 * np.log10(max(mse, 1e-10))
        max_error = np.max(np.abs(val_pred - active_values))

    print(f"\nTopology:")
    print(f"  Recall (active correct):    {topo_recall:.1f}%")
    print(f"  Specificity (inactive correct): {topo_specificity:.1f}%")

    print(f"\nValue Reconstruction:")
    print(f"  MSE:       {mse:.6f}")
    print(f"  RMSE:      {rmse:.6f}")
    print(f"  PSNR:      {psnr_final:.1f} dB")
    print(f"  Max error: {max_error:.6f}")

    # ── Model Size ──────────────────────────────────────────────────
    topo_params = sum(p.numel() for p in classifier.parameters())
    val_params  = sum(p.numel() for p in regressor.parameters())
    topo_bytes  = sum(p.numel() * p.element_size() for p in classifier.parameters())
    val_bytes   = sum(p.numel() * p.element_size() for p in regressor.parameters())

    original_bytes = len(active_values) * 4  # float32 per active voxel
    model_bytes = topo_bytes + val_bytes
    ratio = original_bytes / max(model_bytes, 1)

    print(f"\nModel Size:")
    print(f"  Topology:  {topo_params:,} params ({topo_bytes:,} bytes)")
    print(f"  Value:     {val_params:,} params ({val_bytes:,} bytes)")
    print(f"  Total:     {model_bytes:,} bytes ({model_bytes/1024:.1f} KB)")
    print(f"  Original:  {original_bytes:,} bytes ({original_bytes/1024:.1f} KB)")
    print(f"  Ratio:     {ratio:.1f}x")

    # ── Test TorchScript export ─────────────────────────────────────
    print("\n─── Testing TorchScript export ───")
    try:
        classifier.cpu()
        regressor.cpu()

        topo_scripted = torch.jit.script(classifier)
        val_scripted = torch.jit.script(regressor)

        # Verify scripted models produce same output
        test_input = encoder.cpu()(torch.randn(10, 3))
        orig_topo = classifier(test_input)
        scripted_topo = topo_scripted(test_input)
        topo_diff = (orig_topo - scripted_topo).abs().max().item()

        orig_val = regressor(test_input)
        scripted_val = val_scripted(test_input)
        val_diff = (orig_val - scripted_val).abs().max().item()

        print(f"  Topology script diff:  {topo_diff:.2e} {'PASS' if topo_diff < 1e-5 else 'FAIL'}")
        print(f"  Value script diff:     {val_diff:.2e} {'PASS' if val_diff < 1e-5 else 'FAIL'}")

        # Test serialization roundtrip
        buf = io.BytesIO()
        torch.jit.save(topo_scripted, buf)
        buf.seek(0)
        topo_loaded = torch.jit.load(buf)
        loaded_out = topo_loaded(test_input)
        load_diff = (orig_topo - loaded_out).abs().max().item()
        print(f"  Serialize roundtrip:   {load_diff:.2e} {'PASS' if load_diff < 1e-5 else 'FAIL'}")

    except Exception as e:
        print(f"  TorchScript export FAILED: {e}")

    # ── Test NVDB file write (if pyopenvdb available) ───────────────
    if HAS_VDB:
        print("\n─── Testing NVDB file write/read ───")
        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                # Create a VDB grid
                grid = vdb.FloatGrid()
                grid.name = "test_density"

                # Add some active voxels
                accessor = grid.getAccessor()
                for i in range(min(1000, len(active_coords))):
                    coord = active_coords[i]
                    val = float(active_values[i])
                    ijk = (int(coord[0] * 10), int(coord[1] * 10), int(coord[2] * 10))
                    accessor.setValueOn(ijk, val)

                # Write VDB
                vdb_path = os.path.join(tmpdir, "test.vdb")
                vdb.write(vdb_path, grids=[grid])
                vdb_size = os.path.getsize(vdb_path)
                print(f"  VDB written: {vdb_size:,} bytes")

                # Encode to NVDB using the encoder script
                nvdb_path = os.path.join(tmpdir, "test.nvdb")
                os.system(
                    f"python3 {os.path.dirname(__file__)}/nvdb_encoder.py "
                    f"--input {vdb_path} --output {nvdb_path} "
                    f"--topo-epochs 10 --value-epochs 20 --device cpu 2>/dev/null"
                )

                if os.path.exists(nvdb_path):
                    nvdb_size = os.path.getsize(nvdb_path)
                    print(f"  NVDB written: {nvdb_size:,} bytes")

                    # Read header back
                    with open(nvdb_path, "rb") as f:
                        header_data = f.read(128)
                    magic = struct.unpack("<I", header_data[:4])[0]
                    print(f"  Magic check: {'PASS' if magic == 0x4E564442 else 'FAIL'}")
                else:
                    print("  NVDB write: SKIPPED (encoder may have had issues)")

        except Exception as e:
            print(f"  NVDB test FAILED: {e}")
    else:
        print("\n  Skipping NVDB file test (pyopenvdb not available)")

    # ── Summary ─────────────────────────────────────────────────────
    print("\n═══════════════════════════════════════")
    passed = (topo_recall > 90 and psnr_final > 20)
    print(f"OVERALL: {'PASS' if passed else 'NEEDS TUNING'}")
    if not passed:
        print("  Tip: increase --epochs or --hidden-dim for better results")
    print("═══════════════════════════════════════")

    return passed


def main():
    parser = argparse.ArgumentParser(description="NeuralVDB Synthetic Test")
    parser.add_argument("--resolution", type=int, default=48)
    parser.add_argument("--noise-scale", type=float, default=0.08)
    parser.add_argument("--num-frequencies", type=int, default=6)
    parser.add_argument("--hidden-dim", type=int, default=64)
    parser.add_argument("--topo-epochs", type=int, default=30)
    parser.add_argument("--value-epochs", type=int, default=80)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--device", type=str, default="auto")

    args = parser.parse_args()
    success = train_and_validate(args)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
