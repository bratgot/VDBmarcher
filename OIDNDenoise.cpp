// OIDNDenoise — Intel Open Image Denoise node for Nuke
// Standalone full-frame denoiser using OIDN 2.x
// Created by Marten Blumen
//
// Inputs:
//   0 (beauty) — noisy RGBA render
//   1 (albedo) — optional albedo auxiliary (RGB)
//   2 (normal) — optional normal auxiliary (RGB)

#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>

#include <OpenImageDenoise/oidn.hpp>

#include <vector>
#include <cstdio>
#include <algorithm>

using namespace DD::Image;

static const char* const CLASS = "OIDNDenoise";
static const char* const HELP =
    "OIDNDenoise — Intel Open Image Denoise\n\n"
    "Full-frame AI denoiser for noisy renders.\n"
    "Works on any RGBA input — volumes, path tracers, etc.\n\n"
    "Inputs:\n"
    "  beauty (0) — noisy RGBA image\n"
    "  albedo (1) — optional albedo pass (improves quality)\n"
    "  normal (2) — optional normal pass (improves edge preservation)\n\n"
    "Created by Marten Blumen";

// Helper: fetch a full frame of RGB or RGBA from an Iop into a flat buffer.
static void fetchFrame(Iop* in, int W, int H, ChannelMask mask,
                       std::vector<float>& buf, int chans)
{
    buf.assign((size_t)W * H * chans, 0.f);
    for (int sy = 0; sy < H; ++sy) {
        Row row(0, W);
        in->get(sy, 0, W, mask, row);
        const float* cr = row[Chan_Red];
        const float* cg = row[Chan_Green];
        const float* cb = row[Chan_Blue];
        const float* ca = (chans == 4) ? row[Chan_Alpha] : nullptr;
        for (int ix = 0; ix < W; ++ix) {
            buf[(sy * W + ix) * chans + 0] = cr ? cr[ix] : 0.f;
            buf[(sy * W + ix) * chans + 1] = cg ? cg[ix] : 0.f;
            buf[(sy * W + ix) * chans + 2] = cb ? cb[ix] : 0.f;
            if (ca) buf[(sy * W + ix) * chans + 3] = ca[ix];
        }
    }
}

class OIDNDenoise : public Iop
{
public:
    explicit OIDNDenoise(Node* node) : Iop(node) {}

    void knobs(Knob_Callback f) override;
    const char* Class()     const override { return CLASS; }
    const char* node_help() const override { return HELP; }

    int         minimum_inputs() const override { return 1; }
    int         maximum_inputs() const override { return 3; }
    const char* input_label(int idx, char*) const override {
        switch (idx) {
            case 0: return "beauty";
            case 1: return "albedo";
            case 2: return "normal";
            default: return "";
        }
    }
    bool test_input(int idx, Op* op) const override {
        return dynamic_cast<Iop*>(op) != nullptr;
    }

    void _validate(bool for_real) override;
    void _request(int x, int y, int r, int t, ChannelMask, int count) override;
    void _open() override;
    void _close() override;
    void engine(int y, int x, int r, ChannelMask, Row&) override;
    void append(Hash& hash) override;

    static const Op::Description desc;

private:
    int    _quality    = 1;
    bool   _prefilter  = false;
    bool   _hdr        = true;

    std::vector<float> _outputBuf;
    int  _bufW = 0, _bufH = 0;
};

void OIDNDenoise::knobs(Knob_Callback f)
{
    static const char* qNames[] = {"Fast", "Balanced", "High", nullptr};
    Enumeration_knob(f, &_quality, qNames, "quality", "Quality");
    Tooltip(f, "OIDN filter quality.\n"
               "Fast = lowest latency.\n"
               "Balanced = recommended.\n"
               "High = best quality, slower.");

    Bool_knob(f, &_prefilter, "prefilter", "Pre-filter auxiliaries");
    Tooltip(f, "Pre-denoise albedo and normal buffers before using them\n"
               "as guidance. Improves results when auxiliaries are noisy.");

    Bool_knob(f, &_hdr, "hdr", "HDR");
    Tooltip(f, "Enable HDR mode. Turn on for renders with values > 1.\n"
               "Turn off for pre-tonemapped / LDR images.");
}

void OIDNDenoise::_validate(bool for_real)
{
    if (!input(0)) return;
    copy_info(0);
    set_out_channels(Mask_RGBA);
    if (inputs() > 1 && input(1)) input(1)->validate(for_real);
    if (inputs() > 2 && input(2)) input(2)->validate(for_real);
}

void OIDNDenoise::_request(int x, int y, int r, int t, ChannelMask channels, int count)
{
    if (!input(0)) return;
    const int W = info_.format().width();
    const int H = info_.format().height();
    input(0)->request(0, 0, W, H, Mask_RGBA, count);
    if (inputs() > 1 && input(1)) input(1)->request(0, 0, W, H, Mask_RGB, count);
    if (inputs() > 2 && input(2)) input(2)->request(0, 0, W, H, Mask_RGB, count);
}

void OIDNDenoise::_open()
{
    Iop::_open();

    _bufW = _bufH = 0;
    _outputBuf.clear();

    if (!input(0)) return;
    Iop* beautyIn = dynamic_cast<Iop*>(input(0));
    if (!beautyIn) return;

    const int W = info_.format().width();
    const int H = info_.format().height();
    if (W <= 0 || H <= 0) return;

    try {
        // ── Fetch inputs ──
        std::vector<float> beautyBuf, albedoBuf, normalBuf;

        fetchFrame(beautyIn, W, H, Mask_RGBA, beautyBuf, 4);

        if (inputs() > 1) {
            Iop* in = dynamic_cast<Iop*>(input(1));
            if (in) fetchFrame(in, W, H, Mask_RGB, albedoBuf, 3);
        }
        if (inputs() > 2) {
            Iop* in = dynamic_cast<Iop*>(input(2));
            if (in) fetchFrame(in, W, H, Mask_RGB, normalBuf, 3);
        }

        // ── Run OIDN ──
        _outputBuf.resize((size_t)W * H * 4, 0.f);

        oidn::DeviceRef dev = oidn::newDevice(oidn::DeviceType::CPU);
        dev.commit();

        const char* errMsg = nullptr;
        if (dev.getError(errMsg) != oidn::Error::None) {
            std::fprintf(stderr, "OIDNDenoise: device error: %s\n",
                         errMsg ? errMsg : "(unknown)");
            _outputBuf = beautyBuf;
            _bufW = W; _bufH = H;
            return;
        }

        // Pre-filter auxiliaries
        if (_prefilter) {
            for (auto* buf : {&albedoBuf, &normalBuf}) {
                if (buf->empty()) continue;
                auto f = dev.newFilter("RT");
                f.setImage("color",  buf->data(), oidn::Format::Float3, W, H);
                f.setImage("output", buf->data(), oidn::Format::Float3, W, H);
                f.set("hdr", false);
                f.commit();
                f.execute();
            }
        }

        // Alpha passthrough
        for (int i = 0; i < W * H; ++i)
            _outputBuf[i * 4 + 3] = beautyBuf[i * 4 + 3];

        // Main filter
        oidn::FilterRef filter = dev.newFilter("RT");
        filter.setImage("color",  beautyBuf.data(),  oidn::Format::Float3, W, H, 0, 4 * sizeof(float));
        filter.setImage("output", _outputBuf.data(), oidn::Format::Float3, W, H, 0, 4 * sizeof(float));

        if (!albedoBuf.empty())
            filter.setImage("albedo", albedoBuf.data(), oidn::Format::Float3, W, H);
        if (!normalBuf.empty())
            filter.setImage("normal", normalBuf.data(), oidn::Format::Float3, W, H);

        filter.set("hdr", _hdr);
        static const oidn::Quality kQ[] = {
            oidn::Quality::Fast, oidn::Quality::Balanced, oidn::Quality::High
        };
        filter.set("quality", kQ[std::clamp(_quality, 0, 2)]);
        filter.commit();
        filter.execute();

        if (dev.getError(errMsg) != oidn::Error::None) {
            std::fprintf(stderr, "OIDNDenoise: filter error: %s\n",
                         errMsg ? errMsg : "(unknown)");
        }

        _bufW = W;
        _bufH = H;

    } catch (const std::exception& e) {
        std::fprintf(stderr, "OIDNDenoise: %s\n", e.what());
        _bufW = _bufH = 0;
        _outputBuf.clear();
    } catch (...) {
        std::fprintf(stderr, "OIDNDenoise: unknown exception\n");
        _bufW = _bufH = 0;
        _outputBuf.clear();
    }
}

void OIDNDenoise::_close()
{
    _outputBuf.clear();
    _bufW = _bufH = 0;
    Iop::_close();
}

void OIDNDenoise::append(Hash& hash)
{
    hash.append(_quality);
    hash.append(_prefilter);
    hash.append(_hdr);
}

void OIDNDenoise::engine(int y, int x, int r, ChannelMask channels, Row& row)
{
    if (_outputBuf.empty() || _bufW <= 0 || y < 0 || y >= _bufH) {
        if (input(0)) input0().get(y, x, r, channels, row);
        return;
    }

    float* rO = row.writable(Chan_Red);
    float* gO = row.writable(Chan_Green);
    float* bO = row.writable(Chan_Blue);
    float* aO = row.writable(Chan_Alpha);
    const int W = _bufW;
    for (int ix = x; ix < r && ix < W; ++ix) {
        rO[ix] = _outputBuf[(y * W + ix) * 4 + 0];
        gO[ix] = _outputBuf[(y * W + ix) * 4 + 1];
        bO[ix] = _outputBuf[(y * W + ix) * 4 + 2];
        aO[ix] = _outputBuf[(y * W + ix) * 4 + 3];
    }
}

static Op* build(Node* n) { return new OIDNDenoise(n); }
const Op::Description OIDNDenoise::desc(CLASS, build);
