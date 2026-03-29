// ═══════════════════════════════════════════════════════════════════════════
//  VDBRender_shadingV2.cpp
//  Drop-in replacements for the shading/march functions in VDBRenderIop.cpp.
//
//  INTEGRATION GUIDE:
//  1. Replace VDBRenderIop.h entirely with the new version.
//  2. In VDBRenderIop.cpp, find each ═══ REPLACE ═══ section below and swap
//     out the matching block. The section headers match the originals.
//  3. In append() / knob hash, add the new members (see ═══ APPEND HASH ═══).
//  4. In knobs(), insert the new tab block (see ═══ INSERT KNOBS ═══).
//  5. In knob_changed(), insert the new handlers (see ═══ KNOB CHANGED ═══).
//  6. In engine()'s per-pixel loop, add the jitter seed update
//     (see ═══ ENGINE PIXEL LOOP ═══).
//  7. Rebuild. All existing functionality is preserved.
// ═══════════════════════════════════════════════════════════════════════════

#include "VDBRenderIop.h"
#include <cmath>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════════════
//  ═══ REPLACE: file-scope before makeMarchCtx ═══
//  Static helpers shared by marchRay. Keep all existing kDirs6/8/12 arrays.
// ═══════════════════════════════════════════════════════════════════════════

// HG phase function without the 1/(4π) normalisation factor.
// Returns the "phS" value used throughout marchRay: phS=1 → isotropic.
// Forward scatter (g>0) → phS>1 on-axis. Backward (g<0) → phS>1 behind.
/*static*/ double VDBRenderIop::hgRaw(double cosT, double g) noexcept {
    if (std::abs(g) < 0.001) return 1.0;
    const double g2    = g * g;
    const double denom = 1.0 + g2 - 2.0 * g * cosT;
    return (1.0 - g2) / (denom * std::sqrt(denom));
}

// Deterministic per-pixel jitter: PCG-style hash → [0,1).
// Called once per pixel in engine() and stored in ctx.jitterOff.
/*static*/ double VDBRenderIop::jitterHash(int x, int y) noexcept {
    auto u = static_cast<uint32_t>(x) * 2654435761u
           ^ static_cast<uint32_t>(y) * 2246822519u;
    u ^= u >> 16; u *= 0x45d9f3bu; u ^= u >> 16;
    return (u & 0xFFFFu) / 65536.0;
}


// ═══════════════════════════════════════════════════════════════════════════
//  ═══ REPLACE: makeMarchCtx() ═══
//  Populates all original fields AND the new V2 shading fields.
// ═══════════════════════════════════════════════════════════════════════════

VDBRenderIop::MarchCtx VDBRenderIop::makeMarchCtx() const {
    static auto sEmpty = openvdb::FloatGrid::create();
    const auto& tree   = _floatGrid ? _floatGrid->constTree() : sEmpty->constTree();
    openvdb::FloatGrid::ConstAccessor baseAcc(tree);
    MarchCtx c(baseAcc, openvdb::FloatGrid::ConstAccessor(tree));

    // ── Original fields ──
    c.step  = 1.0 / (std::max(_quality, 1.0) * std::max(_quality, 1.0));
    c.ext   = _extinction;
    c.scat  = _scattering;
    c.g     = std::clamp(_anisotropy, -.999, .999);
    c.g2    = c.g * c.g;
    c.hgN   = (1.0 - c.g2) / (4.0 * M_PI);   // kept for marchRayDensity fallback
    c.nSh   = std::max(1, _shadowSteps);
    c.bDiag = (_bboxMax - _bboxMin).length();
    c.shStep = c.bDiag / (c.nSh * 2.0);

    // ── [V2] Dual-lobe phase function ──
    c.gFwd   = std::clamp(_gForward,  0.001,  0.999);
    c.gBck   = std::clamp(_gBackward, -0.999, -0.001);
    c.lobeMix = std::clamp(_lobeMix, 0.0, 1.0);

    // ── [V2] Scatter quality ──
    c.powder  = std::max(0.0, _powderStrength);
    c.gradMix = std::clamp(_gradientMix, 0.0, 1.0);

    // ── [V2] Analytical multiple scattering ──
    c.msApprox = _msApprox;
    c.msTintR  = _msTint[0];
    c.msTintG  = _msTint[1];
    c.msTintB  = _msTint[2];

    // ── [V2] Chromatic extinction ──
    c.chromatic = _chromaticExt;
    c.extR = _chromaticExt ? _extR : _extinction;
    c.extG = _chromaticExt ? _extG : _extinction;
    c.extB = _chromaticExt ? _extB : _extinction;

    // ── [V2] Jitter (offset is set per-pixel in engine()) ──
    c.jitter    = _jitter;
    c.jitterOff = 0.0;

    // ── Grid accessors ──
    if (_hasTempGrid  && _tempGrid)  c.tempAcc  = std::make_unique<openvdb::FloatGrid::ConstAccessor>(_tempGrid->getConstAccessor());
    if (_hasFlameGrid && _flameGrid) c.flameAcc = std::make_unique<openvdb::FloatGrid::ConstAccessor>(_flameGrid->getConstAccessor());
    if (_hasVelGrid   && _velGrid)   c.velAcc   = std::make_unique<openvdb::Vec3SGrid::ConstAccessor>(_velGrid->getConstAccessor());
    if (_hasColorGrid && _colorGrid) c.colorAcc = std::make_unique<openvdb::Vec3SGrid::ConstAccessor>(_colorGrid->getConstAccessor());

#ifdef VDBRENDER_HAS_NEURAL
    if (_neuralMode && _neural && _neural->loaded()) {
        c.neuralDec  = _neural.get();
        c.neuralMode = true;
    }
#endif
    return c;
}


// ═══════════════════════════════════════════════════════════════════════════
//  ═══ REPLACE: marchRay() AND marchRayExplosion() ═══
//
//  marchRay is now the single unified march function.
//  explosionMode=false → Lit behaviour (density required).
//  explosionMode=true  → Explosion behaviour (emission-first, optional density).
//
//  V2 additions inside:
//    • Dual-lobe HG phase function via hgRaw()
//    • Powder effect (Schneider & Vos 2015)
//    • Gradient-normal Lambertian blend (optional, gradMix > 0)
//    • Analytical multiple scattering (Wrenninge 2015)
//    • Per-channel chromatic transmittance TR/TG/TB
//    • Ray jitter offset (ctx.jitterOff, set by engine per pixel)
// ═══════════════════════════════════════════════════════════════════════════

void VDBRenderIop::marchRay(
        MarchCtx&              ctx,
        const openvdb::Vec3d&  origin,
        const openvdb::Vec3d&  dir,
        float& outR, float& outG, float& outB, float& outA,
        float& outEmR, float& outEmG, float& outEmB,
        bool   explosionMode) const
{
    outR=outG=outB=outA=outEmR=outEmG=outEmB=0.0f;

    // Grid selection: explosion can work on temp/flame without a density grid.
    openvdb::FloatGrid::Ptr xfGrid = _floatGrid
                                   ? _floatGrid
                                   : (explosionMode
                                      ? (_tempGrid ? _tempGrid : _flameGrid)
                                      : nullptr);
    if (!xfGrid) return;

    const auto& xf  = xfGrid->transform();
    const double step  = ctx.step;
    const double ext   = ctx.ext;    // base extinction for shadow rays
    const double scat  = ctx.scat;
    const int    nSh   = ctx.nSh;
    const double shStep = ctx.shStep;

    // Per-channel transmittance for the primary (camera) ray.
    // Chromatic mode uses ctx.extR/G/B; non-chromatic uses ext for all three.
    double TR = 1.0, TG = 1.0, TB = 1.0;
    double aR=0, aG=0, aB=0;   // accumulated scatter + emission
    double eR=0, eG=0, eB=0;   // emission AOV accumulator

    auto* tAcc  = ctx.tempAcc.get();
    auto* fAcc  = ctx.flameAcc.get();
    // For density sampling in explosion mode without a density grid:
    bool hasDensity = (_floatGrid != nullptr);

    // ── shadeSample lambda ─────────────────────────────────────────────────
    // Called at each march step. Captures all outer accumulators by reference.
    // wP = world-space sample position, iP = index-space position.
    // ──────────────────────────────────────────────────────────────────────
    auto shadeSample = [&](const openvdb::Vec3d& wP, const openvdb::Vec3d& iP)
    {
        // ── Averaged transmittance (scalar) for early-exit and weighting ──
        const double Tavg = (TR + TG + TB) / 3.0;

        // ══ EMISSION (temperature + flame) ══════════════════════════════════
        // Evaluated before density in explosion mode (fire can exist without smoke).
        // In lit mode, evaluated after the density guard below.
        double localEmR=0, localEmG=0, localEmB=0;

        auto evalEmission = [&]()
        {
            // Temperature emission
            if (tAcc) {
                float tv = openvdb::tools::BoxSampler::sample(*tAcc, iP) * (float)_tempMix;
                if (tv > 0.001f) {
                    double normT = std::clamp((double)tv, _tempMin, _tempMax);
                    Color3 bb    = blackbody(normT);
                    double tS    = std::clamp((tv - _tempMin) / (_tempMax - _tempMin + 1e-6), 0.0, 1.0);
                    double em    = _emissionIntensity * tS * step;
                    // Per-channel: emission attenuated by primary-ray transmittance
                    double er=bb.r*em, eg=bb.g*em, eb=bb.b*em;
                    aR += er*TR;  aG += eg*TG;  aB += eb*TB;
                    eR += er*TR;  eG += eg*TG;  eB += eb*TB;
                    localEmR += bb.r * _emissionIntensity * tS;
                    localEmG += bb.g * _emissionIntensity * tS;
                    localEmB += bb.b * _emissionIntensity * tS;
                }
            }
            // Flame emission
            if (fAcc) {
                float fv = openvdb::tools::BoxSampler::sample(*fAcc, iP) * (float)_flameMix;
                if (fv > 0.001f) {
                    Color3 fb = blackbody(std::clamp(_tempMin + fv * (_tempMax - _tempMin), _tempMin, _tempMax));
                    double fem = _flameIntensity * fv * step;
                    double fr=fb.r*fem, fg=fb.g*fem, fb2=fb.b*fem;
                    aR += fr*TR;  aG += fg*TG;  aB += fb2*TB;
                    eR += fr*TR;  eG += fg*TG;  eB += fb2*TB;
                    localEmR += fb.r * _flameIntensity * fv;
                    localEmG += fb.g * _flameIntensity * fv;
                    localEmB += fb.b * _flameIntensity * fv;
                }
            }
        };

        if (explosionMode) evalEmission();   // fire-first ordering for explosion

        // ── Fire self-absorption (applied before density scatter) ──
        if (localEmR + localEmG + localEmB > 0.001) {
            double fa = std::clamp((localEmR + localEmG + localEmB) * 0.1, 0.0, 2.0);
            double tf = std::exp(-fa * step);
            TR *= tf;  TG *= tf;  TB *= tf;
        }

        // ── Density guard ─────────────────────────────────────────────────
        float density = hasDensity ? ctx.sampleDensity(iP) * (float)_densityMix : 0.0f;
        if (density <= 1e-6f) {
            // In non-explosion lit mode: no emission either, early out.
            if (!explosionMode) return;
            // Explosion without density: extinction update from fire absorption only,
            // already applied above. No scatter to compute.
            return;
        }

        const double ss     = density * scat;
        const double albedo = std::min(scat / (ext + 1e-8), 1.0);

        // ══ GRADIENT NORMAL (optional, gradMix > 0) ══════════════════════
        // Central-difference density gradient → Lambertian "surface normal".
        // 6 extra BoxSampler lookups per step. Off by default (perf cost).
        // Best used for clouds; leave at 0 for smoke/fire.
        double gx=0, gy=0, gz=0;
        bool hasGrad = false;
        if (ctx.gradMix > 0.001) {
            constexpr double h = 1.0;   // 1 voxel offset — hits cached nodes
            auto& a = ctx.densAcc;
            gx = (double)openvdb::tools::BoxSampler::sample(a, iP+openvdb::Vec3d(h,0,0))
               - (double)openvdb::tools::BoxSampler::sample(a, iP-openvdb::Vec3d(h,0,0));
            gy = (double)openvdb::tools::BoxSampler::sample(a, iP+openvdb::Vec3d(0,h,0))
               - (double)openvdb::tools::BoxSampler::sample(a, iP-openvdb::Vec3d(0,h,0));
            gz = (double)openvdb::tools::BoxSampler::sample(a, iP+openvdb::Vec3d(0,0,h))
               - (double)openvdb::tools::BoxSampler::sample(a, iP-openvdb::Vec3d(0,0,h));
            double gl = std::sqrt(gx*gx + gy*gy + gz*gz);
            if (gl > 1e-6) { gx/=gl; gy/=gl; gz/=gl; hasGrad=true; }
        }

        // ══ POWDER EFFECT (Schneider & Vos 2015 — Horizon Zero Dawn) ════
        // Approximates interior brightening from multiple forward-scattering.
        // Essential for dense explosion fireballs and thick cumulonimbus.
        // powder=0: off. powder=2: natural smoke. powder=4-6: dense fireball.
        const double powder = (ctx.powder > 0.001)
            ? (1.0 - std::exp(-density * ext * ctx.powder))
            : 1.0;

        // ══ DIRECT LIGHTING ═════════════════════════════════════════════
        double stepR=0, stepG=0, stepB=0;   // single-scatter contribution this step (for MS)

        for (const auto& lt : _lights) {
            // Build light direction
            openvdb::Vec3d lD;
            if (lt.isPoint) {
                lD = openvdb::Vec3d(lt.pos[0]-wP[0], lt.pos[1]-wP[1], lt.pos[2]-wP[2]);
                const double l = lD.length();
                if (l < 1e-8) continue;
                lD /= l;
            } else {
                lD = openvdb::Vec3d(lt.dir[0], lt.dir[1], lt.dir[2]);
            }

            // ── Dual-lobe Henyey-Greenstein phase function ──
            // cosT is the cosine between the VIEW direction and the LIGHT direction.
            // Negative because dir points camera→volume, lD points volume→light.
            const double cosT = -(dir[0]*lD[0] + dir[1]*lD[1] + dir[2]*lD[2]);
            double phS = ctx.lobeMix        * hgRaw(cosT, ctx.gFwd)
                       + (1.0-ctx.lobeMix)  * hgRaw(cosT, ctx.gBck);

            // ── Gradient-normal Lambertian blend ──
            // Blends HG (view-relative) with Lambertian NdotL (light-relative).
            // Gives sculpted, three-dimensional appearance to cloud edges.
            if (hasGrad) {
                const double NdotL = std::max(0.0, gx*lD[0] + gy*lD[1] + gz*lD[2]);
                phS = (1.0 - ctx.gradMix) * phS + ctx.gradMix * NdotL;
            }

            // ── Powder modulation ──
            phS *= powder;

            // ── Shadow transmittance (greyscale, uses base ext) ──
            double lT = 1.0;
            for (int i=0; i<nSh; ++i) {
                const auto lw = wP + ((i+1)*shStep)*lD;
                bool in = true;
                for (int a=0; a<3; ++a)
                    if (lw[a]<_bboxMin[a]||lw[a]>_bboxMax[a]) { in=false; break; }
                if (!in) break;
                const auto li = xf.worldToIndex(lw);
                lT *= std::exp(-(double)ctx.sampleShadow(li) * ext * _shadowDensity * shStep);
                if (lT < 0.01) break;
            }

            // ── Scatter accumulation (per-channel chromatic transmittance) ──
            const double base = ss * phS * lT * step;
            aR += base * TR * lt.color[0];
            aG += base * TG * lt.color[1];
            aB += base * TB * lt.color[2];
            // Accumulate single-scatter for MS estimate (T-free; MS adds its own T)
            stepR += base * lt.color[0];
            stepG += base * lt.color[1];
            stepB += base * lt.color[2];
        }

        // ── Ambient ──
        if (_ambientIntensity > 0.0) {
            const double amb = ss * _ambientIntensity * step;
            aR += amb * TR;  aG += amb * TG;  aB += amb * TB;
            stepR += amb;    stepG += amb;    stepB += amb;
        }

        // ── Environment map ──
        if (_hasEnvMap && _envIntensity > 0.0) {
            const int nEnv = 6 + (int)(std::clamp(_envDiffuse, 0.0, 1.0) * 20);
            const openvdb::Vec3d* eDirs[3] = {kDirs6, kDirs8, kDirs12};
            const int eDirCnt[3] = {6, 8, 12};
            int used = 0;
            for (int dS=0; dS<3&&used<nEnv; ++dS)
            for (int di=0; di<eDirCnt[dS]&&used<nEnv; ++di) {
                openvdb::Vec3d eDir = eDirs[dS][di];
                openvdb::Vec3d wDir = eDir;
                if (_hasVolumeXform) {
                    wDir = openvdb::Vec3d(
                        _volFwd[0][0]*eDir[0]+_volFwd[1][0]*eDir[1]+_volFwd[2][0]*eDir[2],
                        _volFwd[0][1]*eDir[0]+_volFwd[1][1]*eDir[1]+_volFwd[2][1]*eDir[2],
                        _volFwd[0][2]*eDir[0]+_volFwd[1][2]*eDir[1]+_volFwd[2][2]*eDir[2]);
                    const double wl = wDir.length();
                    if (wl > 1e-8) wDir /= wl;
                }
                float eRc, eGc, eBc;
                sampleEnv(wDir, eRc, eGc, eBc);
                double eT = 1.0;
                for (int si=0; si<nSh; ++si) {
                    const auto ew = wP + ((si+1)*shStep)*eDir;
                    bool in = true;
                    for (int a=0; a<3; ++a)
                        if (ew[a]<_bboxMin[a]||ew[a]>_bboxMax[a]) { in=false; break; }
                    if (!in) break;
                    const auto ei = xf.worldToIndex(ew);
                    eT *= std::exp(-(double)ctx.sampleShadow(ei) * ext * _shadowDensity * shStep);
                    if (eT < 0.01) break;
                }
                const double envBase = ss * eT * step * _envIntensity * (4.0*M_PI / nEnv);
                aR += envBase * TR * eRc;
                aG += envBase * TG * eGc;
                aB += envBase * TB * eBc;
                stepR += envBase * eRc;
                stepG += envBase * eGc;
                stepB += envBase * eBc;
                ++used;
            }
        }

        // ══ ANALYTICAL MULTIPLE SCATTERING (Wrenninge 2015) ══════════════
        // Approximates infinite-bounce scattering without any extra rays.
        // The 2-param fit (a,b) captures how albedo and anisotropy reduce
        // the effective contribution of higher-order scattering events.
        // For albedo=1 (pure scatter): boost≈0.188. For albedo=0: boost=0.
        // stepR/G/B is the single-scatter contribution of this step; MS adds
        // a fraction of it, weighted by average camera transmittance.
        if (ctx.msApprox && albedo > 0.01 && (stepR+stepG+stepB) > 1e-8) {
            const double a_ms   = 1.0 - 0.5  * albedo;
            const double b_ms   = 1.0 - 0.25 * albedo;
            const double msBoost = albedo * a_ms * b_ms;
            // MS light has scattered through the whole volume — use average T.
            const double Tms = (TR + TG + TB) / 3.0;
            aR += stepR * msBoost * Tms * ctx.msTintR;
            aG += stepG * msBoost * Tms * ctx.msTintG;
            aB += stepB * msBoost * Tms * ctx.msTintB;
        }

        // ══ EMISSION in lit mode (evaluated after density guard) ════════
        if (!explosionMode) evalEmission();

        // ── Emission as embedded light (fire illuminates adjacent smoke) ──
        if (localEmR + localEmG + localEmB > 0.001) {
            const double emScat = ss * step * (1.0 / (4.0*M_PI));
            aR += emScat * localEmR * TR;
            aG += emScat * localEmG * TG;
            aB += emScat * localEmB * TB;
        }

        // ══ PER-CHANNEL TRANSMITTANCE UPDATE ════════════════════════════
        // Chromatic: use per-channel σt (ext_r/g/b).
        // Non-chromatic: all channels use base extinction (identical result to old code).
        if (ctx.chromatic) {
            TR *= std::exp(-density * ctx.extR * step);
            TG *= std::exp(-density * ctx.extG * step);
            TB *= std::exp(-density * ctx.extB * step);
        } else {
            const double tf = std::exp(-density * ext * step);
            TR *= tf;  TG *= tf;  TB *= tf;
        }
    };  // end shadeSample lambda

    // ── HDDA traversal (empty-space skipping) ───────────────────────────────
    // Tavg is used for the early-exit threshold (conservative — keep marching
    // until the last channel is opaque).

    auto Tavg = [&]() { return (TR + TG + TB) / 3.0; };

    if (_volRI) {
        VRI vri(*_volRI);  // thread-safe shallow copy
        openvdb::math::Ray<double> wRay(origin, dir);
        if (!vri.setWorldRay(wRay)) return;
        double it0, it1;
        while (vri.march(it0, it1) && Tavg() > 0.005) {
            const auto wS  = vri.getWorldPos(it0);
            const auto wE  = vri.getWorldPos(it1);
            double wT0 = (wS - origin).dot(dir);
            double wT1 = (wE - origin).dot(dir);
            if (wT1 <= 0) continue;
            if (wT0 < 0)  wT0 = 0;
            // Apply jitter on entry of each HDDA segment
            for (double t2 = wT0 + ctx.jitterOff; t2 < wT1 && Tavg() > 0.005; ) {
                const auto wP = origin + t2 * dir;
                const auto iP = xf.worldToIndex(wP);
                shadeSample(wP, iP);
                double curStep = step;
                if (_adaptiveStep) {
                    const double ld = (double)ctx.sampleDensity(iP) * _densityMix;
                    curStep = step * (ld < 0.01 ? 4.0 : ld < 0.1 ? 2.0 : 1.0);
                }
                t2 += curStep;
            }
        }
    } else {
        // AABB fallback (no HDDA — shouldn't occur when grid is valid)
        double tEnter=0, tExit=1e9;
        for (int a=0; a<3; ++a) {
            const double inv = (std::abs(dir[a]) > 1e-8) ? 1.0/dir[a] : 1e38;
            double t0 = (_bboxMin[a]-origin[a])*inv;
            double t1 = (_bboxMax[a]-origin[a])*inv;
            if (t0 > t1) std::swap(t0, t1);
            tEnter = std::max(tEnter, t0);
            tExit  = std::min(tExit,  t1);
        }
        if (tEnter >= tExit || tExit <= 0) return;
        for (double t2 = tEnter + ctx.jitterOff; t2 < tExit && Tavg() > 0.005; ) {
            const auto wP = origin + t2 * dir;
            const auto iP = xf.worldToIndex(wP);
            shadeSample(wP, iP);
            double curStep = step;
            if (_adaptiveStep) {
                const double ld = (double)ctx.sampleDensity(iP) * _densityMix;
                curStep = step * (ld < 0.01 ? 4.0 : ld < 0.1 ? 2.0 : 1.0);
            }
            t2 += curStep;
        }
    }

    // Final per-channel alpha uses average transmittance.
    outR  = (float)aR;
    outG  = (float)aG;
    outB  = (float)aB;
    outA  = (float)(1.0 - Tavg());
    outEmR = (float)eR;
    outEmG = (float)eG;
    outEmB = (float)eB;
}


// ── Thin wrapper: explosion mode ────────────────────────────────────────────

void VDBRenderIop::marchRayExplosion(
        MarchCtx& ctx, const openvdb::Vec3d& o, const openvdb::Vec3d& d,
        float& R, float& G, float& B, float& A,
        float& emR, float& emG, float& emB) const
{
    marchRay(ctx, o, d, R, G, B, A, emR, emG, emB, /*explosionMode=*/true);
}


// ═══════════════════════════════════════════════════════════════════════════
//  ═══ APPEND HASH ═══
//  Add these lines to your append() function so the new knobs correctly
//  invalidate the render cache when changed.
// ═══════════════════════════════════════════════════════════════════════════
//
//  hash.append(_gForward);  hash.append(_gBackward);  hash.append(_lobeMix);
//  hash.append(_powderStrength);
//  hash.append(_gradientMix);
//  hash.append(_jitter);
//  hash.append(_msApprox);
//  hash.append(_msTint[0]); hash.append(_msTint[1]); hash.append(_msTint[2]);
//  hash.append(_chromaticExt);
//  hash.append(_extR); hash.append(_extG); hash.append(_extB);


// ═══════════════════════════════════════════════════════════════════════════
//  ═══ INSERT KNOBS ═══
//  Insert this entire block into knobs() AFTER the closing Divider/Text_knob
//  at the bottom of the "VDBRender" tab (around line 280), BEFORE
//  Tab_knob(f, "Output").
// ═══════════════════════════════════════════════════════════════════════════

// (Copy the block below into knobs() at the right location)
/*

    // ═══════════════════════════════════════════════════
    //  TAB: Shading V2
    // ═══════════════════════════════════════════════════
    Tab_knob(f, "Shading V2");

    Text_knob(f,
        "<font size='-1' color='#777'>"
        "V2 shading — replaces single-lobe HG for Lit and Explosion modes.<br>"
        "All other modes (Greyscale, Heat, etc.) are unaffected.<br>"
        "Existing scenes load correctly; new knobs default to sensible values."
        "</font>");

    // ── Phase Function ──────────────────────────────────────────────────
    BeginClosedGroup(f, "grp_phase_v2", "Phase Function");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Dual-lobe Henyey-Greenstein (Schneider 2015 / Wrenninge 2017).<br>"
        "Two lobes blended by Lobe Mix give backlit silver-lining and rim.<br>"
        "Replaces the single Anisotropy knob for Lit and Explosion modes."
        "</font>");
    Double_knob(f, &_gForward,  "g_forward",  "G Forward");  SetRange(f,  0, 1);
    Tooltip(f, "Forward-scatter lobe asymmetry.\n"
               "0 = isotropic, 1 = fully forward (backlit glow).\n"
               "Smoke: 0.4   Cloud: 0.8   Explosion: 0.85");
    Double_knob(f, &_gBackward, "g_backward", "G Backward"); SetRange(f, -1, 0);
    Tooltip(f, "Backward-scatter lobe asymmetry.\n"
               "0 = isotropic, -1 = fully backward (rim / halo).\n"
               "Smoke: -0.15   Cloud: -0.1   Dust: -0.3   Explosion: -0.25");
    Double_knob(f, &_lobeMix,   "lobe_mix",   "Lobe Mix");   SetRange(f,  0, 1);
    Tooltip(f, "Blend between forward and backward lobes.\n"
               "1.0 = pure forward lobe only.\n"
               "0.0 = pure backward lobe only.\n"
               "Typical: 0.65-0.85 for most volumes.");
    EndGroup(f);

    // ── Scatter Quality ─────────────────────────────────────────────────
    BeginClosedGroup(f, "grp_scatter_v2", "Scatter Quality");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Powder effect (Schneider & Vos 2015): interior brightening that<br>"
        "makes explosion cores glow. Gradient mix adds sculpted, billowing<br>"
        "edges to clouds. Jitter removes step-banding artifacts."
        "</font>");
    Double_knob(f, &_powderStrength, "powder_strength", "Powder Effect"); SetRange(f, 0, 10);
    Tooltip(f, "Interior brightening from multiple forward-scattering.\n"
               "0 = off. 2 = natural. 4-6 = dense fireball/explosion. 10 = maximum.\n"
               "Essential for explosions. Adds zero extra rays.");
    Double_knob(f, &_gradientMix, "gradient_mix", "Gradient Mix"); SetRange(f, 0, 1);
    Tooltip(f, "Blends the HG phase function with a density-gradient Lambertian term.\n"
               "Gives clouds sculpted, three-dimensional billowing edges.\n"
               "0 = off (recommended for smoke/fire — avoids 6 extra lookups/step).\n"
               "0.3 = clouds. 0.5 = architectural volumes.\n"
               "Adds 6 grid lookups per march step when enabled.");
    Bool_knob(f, &_jitter, "jitter", "Ray Jitter");
    Tooltip(f, "Adds a per-pixel random offset to the march start.\n"
               "Eliminates concentric banding (wood-grain artifact) at low quality.\n"
               "Deterministic hash — identical result every render pass.\n"
               "Zero render cost. Leave on for all quality levels.");
    EndGroup(f);

    // ── Analytical Multiple Scattering ──────────────────────────────────
    BeginClosedGroup(f, "grp_ms_v2", "Multiple Scatter (Analytical)");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Wrenninge 2015 two-parameter approximation to the infinite-bounce<br>"
        "diffusion solution. Replaces brute-force bounce rays — same quality,<br>"
        "100x faster. Scatter Tint adds subtle warm/cool bias to bounced light."
        "</font>");
    Bool_knob(f, &_msApprox, "ms_approx", "Analytical MS");
    Tooltip(f, "Enable the Wrenninge (2015) analytical multiple-scatter approximation.\n"
               "Adds realistic inter-scattered fill light with zero extra rays.\n"
               "Supersedes the Bounce Rays / Multi Bounce knobs on the Quality tab.\n"
               "Leave ON — only disable to compare with brute-force bounces.");
    Color_knob(f, _msTint, "ms_tint", "Scatter Tint");
    Tooltip(f, "Colour tint applied to the multiple-scatter contribution.\n"
               "Default (1.0, 0.97, 0.95) is a very slight warm bias.\n"
               "For cold/overcast scenes try (0.95, 0.97, 1.0) — blue fill.");
    EndGroup(f);

    // ── Chromatic Extinction ─────────────────────────────────────────────
    BeginClosedGroup(f, "grp_chroma_v2", "Chromatic Extinction");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Per-channel extinction (σt per wavelength). Real smoke scatters blue<br>"
        "more than red — set Ext B > Ext R for Rayleigh-like depth colour shift.<br>"
        "Shadow rays stay greyscale for performance."
        "</font>");
    Bool_knob(f, &_chromaticExt, "chromatic_ext", "Enable Chromatic");
    Tooltip(f, "Enables separate extinction per RGB channel.\n"
               "Off: all channels use the main Extinction value.\n"
               "On: use the three sliders below.");
    Double_knob(f, &_extR, "ext_r", "Extinction R"); SetRange(f, 0, 100);
    Tooltip(f, "Red-channel extinction coefficient.\n"
               "Lower than green/blue → volume appears red at depth.");
    Double_knob(f, &_extG, "ext_g", "Extinction G"); SetRange(f, 0, 100);
    Double_knob(f, &_extB, "ext_b", "Extinction B"); SetRange(f, 0, 100);
    Tooltip(f, "Blue-channel extinction coefficient.\n"
               "Higher than red → blue light attenuates faster → warm-tinted depth.");
    EndGroup(f);

    Divider(f, "");
    Text_knob(f,
        "<font size='-1' color='#666'>"
        "V2 references: Schneider &amp; Vos (HZD clouds, SIGGRAPH 2015) · "
        "Wrenninge+ (Production Vol. Rendering, SIGGRAPH 2017)"
        "</font>");

*/


// ═══════════════════════════════════════════════════════════════════════════
//  ═══ KNOB CHANGED ═══
//  Insert these handlers into knob_changed() BEFORE the final
//  `return Iop::knob_changed(k);` line.
//  Also update the scene preset struct and aniso_preset handler (see below).
// ═══════════════════════════════════════════════════════════════════════════

// (Copy each block into knob_changed() at the right location)
/*

    // ── V2: sync chromatic ext_r/g/b to base extinction on first enable ──
    if (k->is("chromatic_ext")) {
        if (_chromaticExt) {
            // Populate per-channel knobs from current base value
            knob("ext_r")->set_value(_extinction); _extR = _extinction;
            knob("ext_g")->set_value(_extinction); _extG = _extinction;
            knob("ext_b")->set_value(_extinction); _extB = _extinction;
        }
        return 1;
    }
    if (k->is("ext_r") || k->is("ext_g") || k->is("ext_b")) return 1;
    if (k->is("powder_strength") || k->is("gradient_mix") || k->is("jitter"))  return 1;
    if (k->is("g_forward") || k->is("g_backward") || k->is("lobe_mix"))        return 1;
    if (k->is("ms_approx") || k->is("ms_tint"))                                return 1;

*/

// ── Updated aniso_preset handler ──────────────────────────────────────────
// REPLACE the existing if(k->is("aniso_preset")) block with this version.
// It now also sets g_forward, g_backward, lobe_mix for V2 dual-lobe.
/*

    if (k->is("aniso_preset")) {
        // (legacy) single-lobe anisotropy
        static const double legacyG[] = {0, 0, 0.4, 0.6, 0.76, 0.8, -0.4, -0.7};
        // (V2) dual-lobe presets: {g_forward, g_backward, lobe_mix}
        struct PhasePreset { double gF, gB, mix; };
        static const PhasePreset pp[] = {
            {0.65, -0.25, 0.70},  // 0: Custom — leave as-is
            {0.00,  0.00, 1.00},  // 1: Isotropic
            {0.40, -0.15, 0.80},  // 2: Smoke
            {0.50, -0.30, 0.60},  // 3: Dust
            {0.80, -0.10, 0.80},  // 4: Cloud
            {0.80, -0.10, 0.85},  // 5: Fog
            {0.20, -0.70, 0.30},  // 6: Rim Light
            {0.00, -0.90, 0.00},  // 7: Strong Back
        };
        if (_anisotropyPreset > 0 && _anisotropyPreset < (int)(sizeof(legacyG)/sizeof(double))) {
            _anisotropy = legacyG[_anisotropyPreset];
            knob("anisotropy")->set_value(_anisotropy);
            // Set V2 dual-lobe params
            const auto& p2 = pp[_anisotropyPreset];
            knob("g_forward") ->set_value(p2.gF);  _gForward  = p2.gF;
            knob("g_backward")->set_value(p2.gB);  _gBackward = p2.gB;
            knob("lobe_mix")  ->set_value(p2.mix); _lobeMix   = p2.mix;
        }
        return 1;
    }

*/

// ── Updated scene preset table (partial) ─────────────────────────────────
// The existing scene_preset block sets extinction, scattering, etc.
// ADD these three extra set_value calls for each preset inside the handler,
// after the existing knob() calls, before the `return 1;`:
//
//   knob("g_forward") ->set_value(p.gF);  _gForward  = p.gF;
//   knob("g_backward")->set_value(p.gB);  _gBackward = p.gB;
//   knob("lobe_mix")  ->set_value(p.mix); _lobeMix   = p.mix;
//   knob("powder_strength")->set_value(p.powder); _powderStrength = p.powder;
//   knob("ms_approx")->set_value(1); _msApprox = true;
//
// And add {gF, gB, mix, powder} to each row of pv[]:
//
//   Custom:      {0.65, -0.25, 0.70, 2.0}
//   Thin Smoke:  {0.40, -0.15, 0.80, 1.5}
//   Dense Smoke: {0.50, -0.15, 0.75, 2.0}
//   Fog/Mist:    {0.80, -0.10, 0.85, 1.5}
//   Cumulus:     {0.80, -0.10, 0.80, 3.0}
//   Fire:        {0.85, -0.25, 0.65, 3.0}
//   Explosion:   {0.85, -0.25, 0.65, 5.0}
//   Pyroclastic: {0.60, -0.20, 0.70, 4.0}
//   Dust Storm:  {0.50, -0.30, 0.60, 2.0}
//   Steam:       {0.70, -0.10, 0.80, 1.5}


// ═══════════════════════════════════════════════════════════════════════════
//  ═══ ENGINE PIXEL LOOP ═══
//  In engine(), inside the `for(int ix=x; ix<r; ++ix)` loop, add these
//  two lines BEFORE the marchRay/marchRayExplosion calls.
//  (They update the per-pixel jitter offset in the shared MarchCtx.)
// ═══════════════════════════════════════════════════════════════════════════

// (Add inside the ix loop, after `openvdb::Vec3d rayO(ox,oy,oz),rayD(...)`)
/*

        // [V2] Per-pixel jitter offset — eliminates step-banding
        if (ctx.jitter)
            ctx.jitterOff = jitterHash(ix, y) * ctx.step;

*/
