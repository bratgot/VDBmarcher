#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>
using namespace DD::Image;
static const char* CLASS = "VDBRender";
class VDBRenderIop : public Iop {
public:
    VDBRenderIop(Node* n) : Iop(n) {}
    const char* Class() const override { return CLASS; }
    const char* node_help() const override { return "test"; }
    void _validate(bool) override { info_.format(format()); set_out_channels(Mask_RGBA); info_.turn_on(Mask_RGBA); }
    void _request(int,int,int,int,ChannelMask,int) override {}
    void engine(int,int,int,ChannelMask,Row& row) override { row.erase(Mask_RGBA); }
    void knobs(Knob_Callback f) override {}
    static const Op::Description desc;
};
static Op* build(Node* n) { return new VDBRenderIop(n); }
const Op::Description VDBRenderIop::desc(CLASS, build);
