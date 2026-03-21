// ═══════════════════════════════════════════════════════════════════════════════
// VDBRender — OpenVDB Volume Ray Marcher for The Foundry Nuke 17
// Created by Marten Blumen
// ═══════════════════════════════════════════════════════════════════════════════

#include "VDBRenderIop.h"
#include <DDImage/Knob.h>
#include <DDImage/Format.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace DD::Image;

const char* VDBRenderIop::CLASS = "VDBRender";
const char* VDBRenderIop::HELP  =
    "VDBRender — OpenVDB Volume Ray Marcher\n\n"
    "A single-scatter volumetric renderer for OpenVDB density grids\n"
    "with temperature-driven emission, multiple lights, and\n"
    "Axis-driven volume transforms.\n\n"
    "Inputs:\n"
    "  0: Camera (required)\n"
    "  1: Axis (optional)\n"
    "  2-9: Lights (optional, up to 8)\n\n"
    "Created by Marten Blumen\n"
    "github.com/bratgot/VDBmarcher";

bool VDBRenderIop::_vdbInitialised = false;

// ═══════════════════════════════════════════════════════════════════════════════
// Blackbody / Color ramps
// ═══════════════════════════════════════════════════════════════════════════════

VDBRenderIop::Color3 VDBRenderIop::blackbody(double T)
{
    if (T < 100.0) T = 100.0;
    double t = T / 100.0;
    double r, g, b;
    if (t<=66.0) r=255.0;
    else {r=329.698727446*std::pow(t-60.0,-0.1332047592); r=std::clamp(r,0.0,255.0);}
    if (t<=66.0) {g=99.4708025861*std::log(t)-161.1195681661; g=std::clamp(g,0.0,255.0);}
    else {g=288.1221695283*std::pow(t-60.0,-0.0755148492); g=std::clamp(g,0.0,255.0);}
    if (t>=66.0) b=255.0;
    else if (t<=19.0) b=0.0;
    else {b=138.5177312231*std::log(t-10.0)-305.0447927307; b=std::clamp(b,0.0,255.0);}
    double bri=std::clamp(std::pow(T/6500.0,0.4),0.0,2.0);
    return{float(r/255.0*bri),float(g/255.0*bri),float(b/255.0*bri)};
}

VDBRenderIop::Color3 VDBRenderIop::evalRamp(ColorScheme s,float t,
                                             const float*gA,const float*gB,
                                             double tMin,double tMax)
{
    t=std::clamp(t,0.0f,1.0f);
    switch(s){
    default: case kLit: case kGreyscale: return{t,t,t};
    case kHeat:
        if(t<0.33f){float s2=t/0.33f;return{s2,0,0};}
        else if(t<0.66f){float s2=(t-0.33f)/0.33f;return{1,s2,0};}
        else{float s2=(t-0.66f)/0.34f;return{1,1,s2};}
    case kCool:
        if(t<0.33f){float s2=t/0.33f;return{0,0,s2};}
        else if(t<0.66f){float s2=(t-0.33f)/0.33f;return{0,s2,1};}
        else{float s2=(t-0.66f)/0.34f;return{s2,1,1};}
    case kBlackbody: return blackbody(tMin+t*(tMax-tMin));
    case kCustomGradient:
        return{gA[0]+t*(gB[0]-gA[0]),gA[1]+t*(gB[1]-gA[1]),gA[2]+t*(gB[2]-gA[2])};
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════════════

VDBRenderIop::VDBRenderIop(Node* node):Iop(node)
{
    inputs(10);
    for(int c=0;c<3;++c)for(int r=0;r<3;++r)_camRot[c][r]=(c==r)?1.0:0.0;
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)_volFwd[i][j]=_volInv[i][j]=(i==j)?1.0:0.0;
    std::memset(_cachedVolFwd,0,sizeof(_cachedVolFwd));
    if(!_vdbInitialised){openvdb::initialize();_vdbInitialised=true;}
}

// ═══════════════════════════════════════════════════════════════════════════════
// Knobs — Single pane, grouped with dividers
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::knobs(Knob_Callback f)
{
    // ── Title + Subheading ──
    Text_knob(f,
        "<font size='+2'><b>VDBRender</b></font>"
        " <font color='#888' size='-1'>v1.0</font>\n"
        "<font color='#aaa'>Single-scatter volume ray marcher for OpenVDB</font>");
    Divider(f,"");

    Format_knob(f, &_formats, "format", "Output Format");
    Tooltip(f,"Resolution. Respects proxy/downres.");

    // ━━━ FILE & SEQUENCE ━━━
    BeginClosedGroup(f, "grp_file", "File & Sequence");
    File_knob(f, &_vdbFilePath, "file", "VDB File");
    Tooltip(f,"Path to .vdb file. Sequences auto-detected.\n"
              "  smoke_0042.vdb → frame number replaced\n"
              "  smoke.####.vdb or smoke.%04d.vdb also work");
    String_knob(f, &_gridName, "grid_name", "Density Grid");
    Tooltip(f,"Float density grid name. Default: density");
    String_knob(f, &_tempGridName, "temp_grid", "Temperature Grid");
    Tooltip(f,"Optional emission grid for fire/explosions.\n"
              "Auto-detects if left empty:\n"
              "  temperature · heat · flame · flames · fire · temp");
    Int_knob(f, &_frameOffset, "frame_offset", "Frame Offset");
    Tooltip(f,"Offset added to timeline frame. Negative = earlier.");
    EndGroup(f);

    // ━━━ RENDER MODE ━━━
    BeginGroup(f, "grp_render", "Render Mode");
    static const char* schemeNames[]={"Lit","Greyscale","Heat","Cool","Blackbody","Custom Gradient",nullptr};
    Enumeration_knob(f, &_colorScheme, schemeNames, "color_scheme", "Mode");
    Tooltip(f,"Lit — single-scatter lighting with shadow rays\n"
              "Greyscale — density → luminance\n"
              "Heat — black → red → yellow → white\n"
              "Cool — black → blue → cyan → white\n"
              "Blackbody — Planckian locus temperature color\n"
              "Custom Gradient — two-color ramp");

    Divider(f, "Blackbody");
    Double_knob(f,&_tempMin,"temp_min","Temp Min (K)");
    SetRange(f,0.0,15000.0);
    Tooltip(f,"Temperature at zero density.\n"
              "Candle: 1800K  Fire: 500K");
    Double_knob(f,&_tempMax,"temp_max","Temp Max (K)");
    SetRange(f,0.0,15000.0);
    Tooltip(f,"Temperature at peak density.\n"
              "Fire: 3000K  Explosion: 8000K  Plasma: 15000K");
    Double_knob(f,&_emissionIntensity,"emission_intensity","Emission Intensity");
    SetRange(f,0.0,50.0);
    Tooltip(f,"Multiplier for temperature grid self-illumination.\n"
              "Only active when a temperature grid is found.");

    Divider(f, "Custom Gradient");
    Color_knob(f,_gradStart,"grad_start","Start Color");
    Color_knob(f,_gradEnd,"grad_end","End Color");
    EndGroup(f);

    // ━━━ VOLUME DENSITY ━━━
    BeginGroup(f, "grp_density", "Volume Density");
    Double_knob(f,&_extinction,"extinction","Extinction");
    SetRange(f,0.0,100.0);
    Tooltip(f,"Light absorption per unit density.\n"
              "Higher = more opaque. Try 1–10 smoke, 5–50 clouds.");
    Double_knob(f,&_scattering,"scattering","Scattering");
    SetRange(f,0.0,100.0);
    Tooltip(f,"In-scatter brightness (Lit mode only).\n"
              "Higher = brighter volume. 0 = pure absorption.");
    Double_knob(f,&_anisotropy,"anisotropy","Anisotropy (g)");
    SetRange(f,-1.0,1.0);
    Tooltip(f,"Henyey-Greenstein phase function.\n"
              "  -1.0 = strong back-scatter (lit from behind)\n"
              "   0.0 = isotropic (uniform scatter)\n"
              "  +1.0 = strong forward-scatter (lit from in front)\n\n"
              "Try 0.3–0.6 for realistic smoke/cloud look.\n"
              "Negative values give a rim-light effect.");
    EndGroup(f);

    // ━━━ LIGHTING ━━━
    BeginGroup(f, "grp_light", "Lighting");
    Text_knob(f,"<font size='-1' color='#999'>Connect Light nodes to inputs 2–9 for scene lighting.\n"
                 "If none connected, the fallback controls below are used.</font>");
    Divider(f, "Fallback Light");
    XYZ_knob(f,_lightDir,"light_dir","Direction");
    Tooltip(f,"Direction FROM volume TOWARD light source.");
    Color_knob(f,_lightColor,"light_color","Color");
    Double_knob(f,&_lightIntensity,"light_intensity","Intensity");
    SetRange(f,0.0,50.0);
    EndGroup(f);

    // ━━━ QUALITY ━━━
    BeginClosedGroup(f, "grp_quality", "Quality");
    Double_knob(f,&_stepSize,"step_size","Step Size");
    SetRange(f,0.01,10.0);
    Tooltip(f,"World-space ray march step.\n"
              "  0.5–2.0   preview / blocking\n"
              "  0.1–0.5   medium quality\n"
              "  0.01–0.1  final render");
    Int_knob(f,&_shadowSteps,"shadow_steps","Shadow Steps");
    Tooltip(f,"Shadow ray samples per light per step.\n"
              "4–8 preview, 16–32 final.");
    EndGroup(f);

    // ━━━ 3D VIEWPORT ━━━
    BeginClosedGroup(f, "grp_viewport", "3D Viewport");
    Bool_knob(f,&_showBbox,"show_bbox","Bounding Box");
    Bool_knob(f,&_showPoints,"show_points","Point Cloud");
    static const char* dLabels[]={"Low (~16k)","Medium (~64k)","High (~250k)",nullptr};
    Enumeration_knob(f,&_pointDensity,dLabels,"point_density","Point Density");
    Double_knob(f,&_pointSize,"point_size","Point Size");
    SetRange(f,1.0,20.0);
    Divider(f,"Viewport Color");
    Bool_knob(f,&_linkViewport,"link_viewport","Link to Render Mode");
    Tooltip(f,"When on, viewport matches the render color scheme.\n"
              "Lit mode falls back to Greyscale in the viewport.");
    static const char*vpNames[]={"Greyscale","Heat","Cool","Blackbody","Custom Gradient",nullptr};
    Enumeration_knob(f,&_viewportColor,vpNames,"viewport_color","Manual Color");
    EndGroup(f);

    // ── Credit + Tech ──
    Divider(f,"");
    Text_knob(f,
        "<font size='-1' color='#666'>Created by Marten Blumen · "
        "github.com/bratgot/VDBmarcher</font>\n"
        "<font size='-1' color='#555'>OpenVDB 12.0 · clang-cl 19 · C++20 · "
        "Nuke NDK 17 · vcpkg · Cam (0) · Axis (1) · Lights (2–9)</font>");
}

int VDBRenderIop::knob_changed(Knob* k)
{
    if(k->is("file")||k->is("grid_name")||k->is("temp_grid")||k->is("frame_offset")){
        _gridValid=false;_previewPoints.clear();return 1;}
    if(k->is("show_points")||k->is("point_density")){
        _previewPoints.clear();return 1;}
    return Iop::knob_changed(k);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Input handling
// ═══════════════════════════════════════════════════════════════════════════════

CameraOp* VDBRenderIop::camera() const{return dynamic_cast<CameraOp*>(Op::input(0));}

AxisOp* VDBRenderIop::axisInput() const{
    if(inputs()<2||!input(1))return nullptr;
    return dynamic_cast<AxisOp*>(Op::input(1));}

const char* VDBRenderIop::input_label(int idx,char*buf) const{
    if(idx==0)return "cam";
    if(idx==1)return "axis";
    if(idx>=2&&idx<=9){std::snprintf(buf,10,"light%d",idx-1);return buf;}
    return nullptr;}

bool VDBRenderIop::test_input(int idx,Op*op) const{
    if(idx==0)return dynamic_cast<CameraOp*>(op)||Iop::test_input(idx,op);
    if(idx==1)return dynamic_cast<AxisOp*>(op)!=nullptr;
    if(idx>=2)return dynamic_cast<LightOp*>(op)!=nullptr;
    return Iop::test_input(idx,op);}

Op* VDBRenderIop::default_input(int idx) const{
    if(idx>=1)return nullptr;
    return Iop::default_input(idx);}

// ═══════════════════════════════════════════════════════════════════════════════
// Frame sequence
// ═══════════════════════════════════════════════════════════════════════════════

std::string VDBRenderIop::resolveFramePath(int frame) const
{
    std::string path(_vdbFilePath?_vdbFilePath:"");
    if(path.empty())return path;
    size_t h=path.find('#');
    if(h!=std::string::npos){size_t he=h;while(he<path.size()&&path[he]=='#')++he;
        char buf[64];std::snprintf(buf,sizeof(buf),"%0*d",(int)(he-h),frame);
        path.replace(h,he-h,buf);return path;}
    size_t p=path.find('%');
    if(p!=std::string::npos){size_t s=p+1;if(s<path.size()&&path[s]=='0')++s;
        while(s<path.size()&&path[s]>='0'&&path[s]<='9')++s;
        if(s<path.size()&&path[s]=='d'){char buf[64];
            std::snprintf(buf,sizeof(buf),path.substr(p,s-p+1).c_str(),frame);
            path.replace(p,s-p+1,buf);return path;}}
    size_t dot=path.rfind('.');if(dot==std::string::npos)dot=path.size();
    size_t ne=dot,ns=ne;while(ns>0&&path[ns-1]>='0'&&path[ns-1]<='9')--ns;
    if(ns<ne){char buf[64];std::snprintf(buf,sizeof(buf),"%0*d",(int)(ne-ns),frame);
        path.replace(ns,ne-ns,buf);return path;}
    return path;
}

// ═══════════════════════════════════════════════════════════════════════════════
// append — hash must change when lights/axis move
// We validate upstream 3D ops in _validate(for_real=false) which runs BEFORE
// append(). Then we can safely read their hashes here.
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::append(Hash& hash)
{
    hash.append(outputContext().frame());
    hash.append(_frameOffset);
    hash.append(_colorScheme);
    hash.append(_emissionIntensity);
    hash.append(_lightIntensity);
    hash.append(_anisotropy);
    for(int i=0;i<3;++i){hash.append(_lightDir[i]);hash.append(_lightColor[i]);}

    // Axis and light hashes — these ops were validated in _validate(false)
    for(int idx=1;idx<inputs();++idx){
        Op*op=Op::input(idx);
        if(op) hash.append(op->hash());
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3D Viewport
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::build_handles(ViewerContext*ctx){
    if(!_gridValid||(!_showBbox&&!_showPoints))return;
    add_draw_handle(ctx);}

void VDBRenderIop::rebuildPointCloud()
{
    _previewPoints.clear();_maxDensity=1.0f;
    if(!_floatGrid)return;
    int res=24;if(_pointDensity==1)res=40;if(_pointDensity==2)res=64;
    auto acc=_floatGrid->getConstAccessor();
    const auto&xf=_floatGrid->transform();
    double dx=(_bboxMax[0]-_bboxMin[0])/res,dy=(_bboxMax[1]-_bboxMin[1])/res,dz=(_bboxMax[2]-_bboxMin[2])/res;
    _previewPoints.reserve(res*res*res/4);
    float maxD=0;
    for(int iz=0;iz<res;++iz){double wz=_bboxMin[2]+(iz+0.5)*dz;
    for(int iy=0;iy<res;++iy){double wy=_bboxMin[1]+(iy+0.5)*dy;
    for(int ix=0;ix<res;++ix){double wx=_bboxMin[0]+(ix+0.5)*dx;
        auto ip=xf.worldToIndex(openvdb::Vec3d(wx,wy,wz));
        float d=acc.getValue(openvdb::Coord((int)std::floor(ip[0]),(int)std::floor(ip[1]),(int)std::floor(ip[2])));
        if(d>0.001f){
            float px=(float)wx,py=(float)wy,pz=(float)wz;
            if(_hasVolumeXform){
                px=(float)(_volFwd[0][0]*wx+_volFwd[1][0]*wy+_volFwd[2][0]*wz+_volFwd[3][0]);
                py=(float)(_volFwd[0][1]*wx+_volFwd[1][1]*wy+_volFwd[2][1]*wz+_volFwd[3][1]);
                pz=(float)(_volFwd[0][2]*wx+_volFwd[1][2]*wy+_volFwd[2][2]*wz+_volFwd[3][2]);}
            _previewPoints.push_back({px,py,pz,d});
            if(d>maxD)maxD=d;}
    }}} _maxDensity=(maxD>0)?maxD:1.0f;
}

void VDBRenderIop::draw_handle(ViewerContext*ctx)
{
    if(!_gridValid)return;
    glPushAttrib(GL_CURRENT_BIT|GL_LINE_BIT|GL_ENABLE_BIT|GL_POINT_BIT);
    glDisable(GL_LIGHTING);

    if(_showBbox){
        float corners[8][3];int ci=0;
        for(int iz=0;iz<=1;++iz)for(int iy=0;iy<=1;++iy)for(int ix=0;ix<=1;++ix){
            double wx=ix?_bboxMax[0]:_bboxMin[0],wy=iy?_bboxMax[1]:_bboxMin[1],wz=iz?_bboxMax[2]:_bboxMin[2];
            if(_hasVolumeXform){
                double tx=_volFwd[0][0]*wx+_volFwd[1][0]*wy+_volFwd[2][0]*wz+_volFwd[3][0];
                double ty=_volFwd[0][1]*wx+_volFwd[1][1]*wy+_volFwd[2][1]*wz+_volFwd[3][1];
                double tz=_volFwd[0][2]*wx+_volFwd[1][2]*wy+_volFwd[2][2]*wz+_volFwd[3][2];
                wx=tx;wy=ty;wz=tz;}
            corners[ci][0]=(float)wx;corners[ci][1]=(float)wy;corners[ci][2]=(float)wz;++ci;}
        glLineWidth(1.5f);glColor3f(0,1,0);
        glBegin(GL_LINES);
        for(int e=0;e<12;++e){glVertex3fv(corners[_bboxEdges[e][0]]);glVertex3fv(corners[_bboxEdges[e][1]]);}
        glEnd();
        float cx=0,cy=0,cz=0;
        for(int i=0;i<8;++i){cx+=corners[i][0];cy+=corners[i][1];cz+=corners[i][2];}
        cx/=8;cy/=8;cz/=8;float sz=0;
        for(int i=0;i<8;++i){float d=std::sqrt((corners[i][0]-cx)*(corners[i][0]-cx)+(corners[i][1]-cy)*(corners[i][1]-cy)+(corners[i][2]-cz)*(corners[i][2]-cz));if(d>sz)sz=d;}
        sz*=0.05f;glColor3f(1,1,0);glBegin(GL_LINES);
        glVertex3f(cx-sz,cy,cz);glVertex3f(cx+sz,cy,cz);
        glVertex3f(cx,cy-sz,cz);glVertex3f(cx,cy+sz,cz);
        glVertex3f(cx,cy,cz-sz);glVertex3f(cx,cy,cz+sz);
        glEnd();
    }

    if(_showPoints&&_floatGrid){
        int cf=(int)outputContext().frame()+_frameOffset;
        std::string cp=resolveFramePath(cf);
        if(_previewPoints.empty()||_cachedPointDensity!=_pointDensity||_cachedPointsPath!=cp
           ||_cachedPointsFrame!=cf||_cachedHasXform!=_hasVolumeXform
           ||std::memcmp(_cachedVolFwd,_volFwd,sizeof(_volFwd))!=0){
            rebuildPointCloud();_cachedPointDensity=_pointDensity;_cachedPointsPath=cp;
            _cachedPointsFrame=cf;_cachedHasXform=_hasVolumeXform;
            std::memcpy(_cachedVolFwd,_volFwd,sizeof(_volFwd));}
        if(!_previewPoints.empty()){
            ColorScheme vpScheme;
            if(_linkViewport)vpScheme=(_colorScheme==kLit)?kGreyscale:static_cast<ColorScheme>(_colorScheme);
            else switch(_viewportColor){default:case 0:vpScheme=kGreyscale;break;case 1:vpScheme=kHeat;break;
                case 2:vpScheme=kCool;break;case 3:vpScheme=kBlackbody;break;case 4:vpScheme=kCustomGradient;break;}
            float gA[3]={(float)_gradStart[0],(float)_gradStart[1],(float)_gradStart[2]};
            float gB[3]={(float)_gradEnd[0],(float)_gradEnd[1],(float)_gradEnd[2]};
            float invMax=1.0f/_maxDensity;
            glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);
            glPointSize((float)_pointSize);glEnable(GL_POINT_SMOOTH);
            glBegin(GL_POINTS);
            for(const auto&pt:_previewPoints){float t=std::min(pt.density*invMax,1.0f);
                Color3 c=evalRamp(vpScheme,t,gA,gB,_tempMin,_tempMax);
                glColor4f(c.r,c.g,c.b,0.15f+0.85f*t);glVertex3f(pt.x,pt.y,pt.z);}
            glEnd();glDisable(GL_BLEND);}
    }
    glPopAttrib();
}

// ═══════════════════════════════════════════════════════════════════════════════
// _validate — CRITICAL: validate upstream 3D ops early so append() can hash them
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::_validate(bool for_real)
{
    _camValid=false;_hasVolumeXform=false;_lights.clear();

    // ── EARLY: validate ALL upstream 3D ops so their hashes are current ──
    // This runs on BOTH for_real=false and for_real=true passes.
    // The false pass happens before append(), so light hashes will be fresh.
    for(int idx=0;idx<inputs();++idx){
        Op*op=Op::input(idx);
        if(op) op->validate(for_real);
    }

    // ── Format ──
    const Format*fmt=_formats.format();const Format*full=_formats.fullSizeFormat();
    if(!full)full=fmt;
    if(fmt){info_.format(*fmt);info_.full_size_format(full?*full:*fmt);info_.set(*fmt);}
    info_.channels(Mask_RGBA);set_out_channels(Mask_RGBA);info_.turn_on(Mask_RGBA);

    if(!for_real) return;

    // ── Camera ──
    if(CameraOp*cam=camera()){
        const auto cw=cam->worldTransform();
        _camOrigin=openvdb::Vec3d(cw[3][0],cw[3][1],cw[3][2]);
        for(int c=0;c<3;++c)for(int r=0;r<3;++r)_camRot[c][r]=(double)cw[c][r];
        _halfW=(double)cam->horizontalAperture()*0.5/(double)cam->focalLength();
        _camValid=true;
    }else{error("Connect a Camera to input 0.");return;}

    // ── Axis ──
    if(AxisOp*axis=axisInput()){
        const auto axM=axis->worldTransform();
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)_volFwd[c][r]=(double)axM[c][r];
        DD::Image::Matrix4 m4;
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)m4[c][r]=(float)_volFwd[c][r];
        auto inv=m4.inverse();
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)_volInv[c][r]=(double)inv[c][r];
        _hasVolumeXform=true;
    }else{
        for(int i=0;i<4;++i)for(int j=0;j<4;++j)_volFwd[i][j]=_volInv[i][j]=(i==j)?1.0:0.0;}

    // ── Gather lights from inputs 2–9 ──
    for(int idx=2;idx<inputs();++idx){
        if(!input(idx))continue;
        LightOp*light=dynamic_cast<LightOp*>(Op::input(idx));
        if(!light)continue;

        const auto lm=light->worldTransform();
        CachedLight cl;

        // Store light world-space position (translation column)
        cl.pos[0]=(double)lm[3][0];
        cl.pos[1]=(double)lm[3][1];
        cl.pos[2]=(double)lm[3][2];
        cl.isPoint=true;  // always compute direction from position per-sample

        // Also store Z-axis direction as fallback
        cl.dir[0]=(double)lm[2][0];
        cl.dir[1]=(double)lm[2][1];
        cl.dir[2]=(double)lm[2][2];
        double len=std::sqrt(cl.dir[0]*cl.dir[0]+cl.dir[1]*cl.dir[1]+cl.dir[2]*cl.dir[2]);
        if(len>1e-8){cl.dir[0]/=len;cl.dir[1]/=len;cl.dir[2]/=len;}

        // Color and intensity from knobs
        double lr=1,lg=1,lb=1,intensity=1;
        if(Knob*ck=light->knob("color")){
            lr=ck->get_value_at(outputContext().frame(),0);
            lg=ck->get_value_at(outputContext().frame(),1);
            lb=ck->get_value_at(outputContext().frame(),2);}
        if(Knob*ik=light->knob("intensity"))
            intensity=ik->get_value_at(outputContext().frame());

        cl.color[0]=lr*intensity;
        cl.color[1]=lg*intensity;
        cl.color[2]=lb*intensity;

        std::printf("VDBRender: Light[%d] pos=(%.1f,%.1f,%.1f) dir=(%.3f,%.3f,%.3f) color=(%.2f,%.2f,%.2f) int=%.2f\n",
            idx-2, cl.pos[0],cl.pos[1],cl.pos[2], cl.dir[0],cl.dir[1],cl.dir[2], lr,lg,lb, intensity);

        _lights.push_back(cl);
    }

    // Fallback
    if(_lights.empty()){
        CachedLight cl;
        cl.dir[0]=_lightDir[0];cl.dir[1]=_lightDir[1];cl.dir[2]=_lightDir[2];
        double len=std::sqrt(cl.dir[0]*cl.dir[0]+cl.dir[1]*cl.dir[1]+cl.dir[2]*cl.dir[2]);
        if(len>1e-8){cl.dir[0]/=len;cl.dir[1]/=len;cl.dir[2]/=len;}
        cl.color[0]=_lightColor[0]*_lightIntensity;
        cl.color[1]=_lightColor[1]*_lightIntensity;
        cl.color[2]=_lightColor[2]*_lightIntensity;
        cl.isPoint=false;
        cl.pos[0]=cl.pos[1]=cl.pos[2]=0;
        _lights.push_back(cl);
        std::printf("VDBRender: Using fallback light dir=(%.3f,%.3f,%.3f) color=(%.2f,%.2f,%.2f)\n",
            cl.dir[0],cl.dir[1],cl.dir[2],cl.color[0],cl.color[1],cl.color[2]);
    }

    // Transform light directions + positions into volume-local space
    if(_hasVolumeXform){
        for(auto&cl:_lights){
            // Transform direction (rotation only)
            double dx=_volInv[0][0]*cl.dir[0]+_volInv[1][0]*cl.dir[1]+_volInv[2][0]*cl.dir[2];
            double dy=_volInv[0][1]*cl.dir[0]+_volInv[1][1]*cl.dir[1]+_volInv[2][1]*cl.dir[2];
            double dz=_volInv[0][2]*cl.dir[0]+_volInv[1][2]*cl.dir[1]+_volInv[2][2]*cl.dir[2];
            double l=std::sqrt(dx*dx+dy*dy+dz*dz);
            if(l>1e-8){dx/=l;dy/=l;dz/=l;}
            cl.dir[0]=dx;cl.dir[1]=dy;cl.dir[2]=dz;

            // Transform position (full affine)
            if(cl.isPoint){
                double px=_volInv[0][0]*cl.pos[0]+_volInv[1][0]*cl.pos[1]+_volInv[2][0]*cl.pos[2]+_volInv[3][0];
                double py=_volInv[0][1]*cl.pos[0]+_volInv[1][1]*cl.pos[1]+_volInv[2][1]*cl.pos[2]+_volInv[3][1];
                double pz=_volInv[0][2]*cl.pos[0]+_volInv[1][2]*cl.pos[1]+_volInv[2][2]*cl.pos[2]+_volInv[3][2];
                cl.pos[0]=px;cl.pos[1]=py;cl.pos[2]=pz;
            }
        }
    }

    // ── Load VDB ──
    {
        Guard guard(_loadLock);
        int curFrame=(int)outputContext().frame()+_frameOffset;
        std::string path=resolveFramePath(curFrame);
        std::string grid(_gridName?_gridName:"");

        {std::string tp=path;for(auto&c:tp)if(c=='\\')c='/';
         FILE*f=fopen(tp.c_str(),"rb");
         if(!f){std::string orig(_vdbFilePath?_vdbFilePath:"");
             if(orig!=path){for(auto&c:orig)if(c=='\\')c='/';
                 FILE*of=fopen(orig.c_str(),"rb");if(of){fclose(of);path=orig;}}}
         else fclose(f);}

        if(!_gridValid||path!=_loadedPath||grid!=_loadedGrid||curFrame!=_loadedFrame){
            _floatGrid.reset();_tempGrid.reset();
            _gridValid=false;_hasTempGrid=false;_previewPoints.clear();

            if(!path.empty()){
                try{
                    std::string cp=path;for(auto&c:cp)if(c=='\\')c='/';
                    openvdb::io::File file(cp);file.open();
                    std::string target=grid.empty()?"density":grid;
                    openvdb::GridBase::Ptr bg;bool found=false;
                    for(auto it=file.beginName();it!=file.endName();++it)
                        if(it.gridName()==target){bg=file.readGrid(target);found=true;break;}
                    if(!found)for(auto it=file.beginName();it!=file.endName();++it){
                        auto g=file.readGrid(it.gridName());
                        if(g->isType<openvdb::FloatGrid>()){bg=g;found=true;break;}}

                    std::string tgName(_tempGridName?_tempGridName:"");
                    openvdb::GridBase::Ptr tbg;
                    if(!tgName.empty()){for(auto it=file.beginName();it!=file.endName();++it)
                        if(it.gridName()==tgName){tbg=file.readGrid(tgName);break;}}
                    else{static const char*tN[]={"temperature","heat","flame","flames","fire","temp",nullptr};
                        for(int i=0;tN[i];++i){for(auto it=file.beginName();it!=file.endName();++it)
                            if(it.gridName()==tN[i]){tbg=file.readGrid(tN[i]);break;}
                            if(tbg)break;}}

                    file.close();
                    if(!found||!bg){error("No float grid '%s'.",target.c_str());return;}
                    if(!bg->isType<openvdb::FloatGrid>()){error("Grid not float.");return;}
                    _floatGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(bg);
                    if(tbg&&tbg->isType<openvdb::FloatGrid>()){
                        _tempGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(tbg);_hasTempGrid=true;
                        std::printf("VDBRender: Loaded temperature grid\n");}

                    auto ab=_floatGrid->evalActiveVoxelBoundingBox();
                    if(ab.empty()){error("No active voxels.");return;}
                    const auto&xf=_floatGrid->transform();
                    const auto&lo=ab.min();const auto&hi=ab.max();
                    openvdb::Vec3d corners[8];int ci2=0;
                    for(int iz=0;iz<=1;++iz)for(int iy=0;iy<=1;++iy)for(int ix=0;ix<=1;++ix)
                        corners[ci2++]=xf.indexToWorld(openvdb::Vec3d(ix?hi.x()+1.0:lo.x(),iy?hi.y()+1.0:lo.y(),iz?hi.z()+1.0:lo.z()));
                    _bboxMin=_bboxMax=corners[0];
                    for(int i=1;i<8;++i)for(int a=0;a<3;++a){
                        _bboxMin[a]=std::min(_bboxMin[a],corners[i][a]);
                        _bboxMax[a]=std::max(_bboxMax[a],corners[i][a]);}

                    _gridValid=true;_loadedPath=path;_loadedGrid=grid;_loadedFrame=curFrame;
                }catch(const std::exception&e){error("OpenVDB: %s",e.what());}
                 catch(...){error("OpenVDB: unknown error.");}
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// engine
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::_request(int,int,int,int,ChannelMask,int){}

void VDBRenderIop::engine(int y,int x,int r,ChannelMask channels,Row&row)
{
    if(!_gridValid||!_floatGrid||!_camValid){
        foreach(z,channels){float*p=row.writable(z);for(int i=x;i<r;++i)p[i]=0;}return;}

    const Format&fmt=format();
    int W=fmt.width(),H=fmt.height();
    double halfW=_halfW,halfH=_halfW*(double)H/(double)W;
    float*rOut=row.writable(Chan_Red),*gOut=row.writable(Chan_Green);
    float*bOut=row.writable(Chan_Blue),*aOut=row.writable(Chan_Alpha);

    ColorScheme scheme=static_cast<ColorScheme>(_colorScheme);
    float gA[3]={(float)_gradStart[0],(float)_gradStart[1],(float)_gradStart[2]};
    float gB[3]={(float)_gradEnd[0],(float)_gradEnd[1],(float)_gradEnd[2]};

    for(int ix=x;ix<r;++ix){
        double ndcX=(ix+0.5)/(double)W*2.0-1.0,ndcY=(y+0.5)/(double)H*2.0-1.0;
        double rcx=ndcX*halfW,rcy=ndcY*halfH,rcz=-1.0;
        double rdx=_camRot[0][0]*rcx+_camRot[1][0]*rcy+_camRot[2][0]*rcz;
        double rdy=_camRot[0][1]*rcx+_camRot[1][1]*rcy+_camRot[2][1]*rcz;
        double rdz=_camRot[0][2]*rcx+_camRot[1][2]*rcy+_camRot[2][2]*rcz;
        double len=std::sqrt(rdx*rdx+rdy*rdy+rdz*rdz);
        if(len>1e-8){rdx/=len;rdy/=len;rdz/=len;}

        double ox=_camOrigin[0],oy=_camOrigin[1],oz=_camOrigin[2];
        if(_hasVolumeXform){
            double tx=_volInv[0][0]*ox+_volInv[1][0]*oy+_volInv[2][0]*oz+_volInv[3][0];
            double ty=_volInv[0][1]*ox+_volInv[1][1]*oy+_volInv[2][1]*oz+_volInv[3][1];
            double tz=_volInv[0][2]*ox+_volInv[1][2]*oy+_volInv[2][2]*oz+_volInv[3][2];
            ox=tx;oy=ty;oz=tz;
            double dx2=_volInv[0][0]*rdx+_volInv[1][0]*rdy+_volInv[2][0]*rdz;
            double dy2=_volInv[0][1]*rdx+_volInv[1][1]*rdy+_volInv[2][1]*rdz;
            double dz2=_volInv[0][2]*rdx+_volInv[1][2]*rdy+_volInv[2][2]*rdz;
            len=std::sqrt(dx2*dx2+dy2*dy2+dz2*dz2);
            if(len>1e-8){dx2/=len;dy2/=len;dz2/=len;}
            rdx=dx2;rdy=dy2;rdz=dz2;}

        openvdb::Vec3d rayO(ox,oy,oz),rayD(rdx,rdy,rdz);
        if(scheme==kLit){float R=0,G=0,B=0,A=0;marchRay(rayO,rayD,R,G,B,A);
            rOut[ix]=R;gOut[ix]=G;bOut[ix]=B;aOut[ix]=A;
        }else{float density=0,alpha=0;marchRayDensity(rayO,rayD,density,alpha);
            Color3 c=evalRamp(scheme,density,gA,gB,_tempMin,_tempMax);
            rOut[ix]=c.r*alpha;gOut[ix]=c.g*alpha;bOut[ix]=c.b*alpha;aOut[ix]=alpha;}
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// marchRay — multi-light single-scatter
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::marchRay(const openvdb::Vec3d&origin,const openvdb::Vec3d&dir,
                             float&outR,float&outG,float&outB,float&outA) const
{
    outR=outG=outB=outA=0;
    double tEnter=0,tExit=1e9;
    for(int a=0;a<3;++a){
        double inv=(std::abs(dir[a])>1e-8)?1.0/dir[a]:1e38;
        double t0=(_bboxMin[a]-origin[a])*inv,t1=(_bboxMax[a]-origin[a])*inv;
        if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}
    if(tEnter>=tExit||tExit<=0)return;

    auto acc=_floatGrid->getConstAccessor();
    const auto&xf=_floatGrid->transform();
    double step=_stepSize,ext=_extinction,scat=_scattering;
    double g=std::clamp(_anisotropy,-0.999,0.999);
    int nShadow=std::max(1,_shadowSteps);
    double bboxDiag=(_bboxMax-_bboxMin).length();
    double shadowStep=bboxDiag/(nShadow*2.0);
    double T=1.0,aR=0,aG=0,aB=0;

    std::unique_ptr<openvdb::FloatGrid::ConstAccessor> tempAcc;
    if(_hasTempGrid&&_tempGrid)
        tempAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_tempGrid->getConstAccessor());

    // Precompute HG constants
    double g2=g*g;
    double hgNorm=(1.0-g2)/(4.0*M_PI);

    for(double t=tEnter;t<tExit&&T>0.001;t+=step){
        auto wPos=origin+t*dir;
        auto iPos=xf.worldToIndex(wPos);
        openvdb::Coord ijk((int)std::floor(iPos[0]),(int)std::floor(iPos[1]),(int)std::floor(iPos[2]));
        float density=acc.getValue(ijk);

        if(density>1e-6f){
            double se=density*ext;
            double ss=density*scat;

            // ── Per-light scatter + shadow ──
            for(const auto&light:_lights){
                // Compute light direction for this sample point
                openvdb::Vec3d lDir;
                if(light.isPoint){
                    // Point light: direction from sample toward light position
                    lDir=openvdb::Vec3d(light.pos[0]-wPos[0],
                                        light.pos[1]-wPos[1],
                                        light.pos[2]-wPos[2]);
                    double len=lDir.length();
                    if(len>1e-8) lDir/=len;
                    else lDir=openvdb::Vec3d(0,1,0);
                }else{
                    // Directional: constant direction
                    lDir=openvdb::Vec3d(light.dir[0],light.dir[1],light.dir[2]);
                }

                // Henyey-Greenstein phase function
                // cosTheta = dot(viewDir, lightDir)
                // viewDir = -dir (we march along dir, camera looks along -dir)
                double cosTheta=-(dir[0]*lDir[0]+dir[1]*lDir[1]+dir[2]*lDir[2]);
                double phase;
                if(std::abs(g)<0.001){
                    phase=1.0/(4.0*M_PI); // isotropic
                }else{
                    double denom=1.0+g2-2.0*g*cosTheta;
                    phase=hgNorm/std::pow(denom,1.5);
                }
                // Normalise so isotropic (g=0) gives phase=1 contribution
                double phaseScale=phase*4.0*M_PI;

                // Shadow ray toward light
                double lightT=1.0;
                {auto la=_floatGrid->getConstAccessor();
                 for(int i=0;i<nShadow;++i){
                    auto lw=wPos+((i+1)*shadowStep)*lDir;
                    bool inside=true;
                    for(int a=0;a<3;++a)if(lw[a]<_bboxMin[a]||lw[a]>_bboxMax[a]){inside=false;break;}
                    if(!inside)break;
                    auto li=xf.worldToIndex(lw);
                    lightT*=std::exp(-(double)la.getValue(
                        openvdb::Coord((int)std::floor(li[0]),(int)std::floor(li[1]),(int)std::floor(li[2])))*ext*shadowStep);
                    if(lightT<0.001)break;}}

                double contrib=ss*phaseScale*lightT*T*step;
                aR+=contrib*light.color[0];
                aG+=contrib*light.color[1];
                aB+=contrib*light.color[2];
            }

            // Temperature emission
            if(tempAcc){float tv=tempAcc->getValue(ijk);
                if(tv>0.01f){auto normT=std::clamp((double)tv,_tempMin,_tempMax);
                    Color3 bb=blackbody(normT);
                    double em=tv*_emissionIntensity*T*step*0.01;
                    aR+=bb.r*em;aG+=bb.g*em;aB+=bb.b*em;}}

            T*=std::exp(-se*step);
        }
    }
    outR=(float)aR;outG=(float)aG;outB=(float)aB;outA=(float)(1.0-T);
}

// ═══════════════════════════════════════════════════════════════════════════════
// marchRayDensity
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::marchRayDensity(const openvdb::Vec3d&origin,const openvdb::Vec3d&dir,
                                    float&outDensity,float&outAlpha) const
{
    outDensity=0;outAlpha=0;
    double tEnter=0,tExit=1e9;
    for(int a=0;a<3;++a){
        double inv=(std::abs(dir[a])>1e-8)?1.0/dir[a]:1e38;
        double t0=(_bboxMin[a]-origin[a])*inv,t1=(_bboxMax[a]-origin[a])*inv;
        if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}
    if(tEnter>=tExit||tExit<=0)return;
    auto acc=_floatGrid->getConstAccessor();
    const auto&xf=_floatGrid->transform();
    bool useTG=_hasTempGrid&&_tempGrid&&_colorScheme==kBlackbody;
    std::unique_ptr<openvdb::FloatGrid::ConstAccessor> tAcc;
    if(useTG)tAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_tempGrid->getConstAccessor());
    double step=_stepSize,ext=_extinction,T=1.0,wV=0;
    for(double t=tEnter;t<tExit&&T>0.001;t+=step){
        auto wPos=origin+t*dir;auto iPos=xf.worldToIndex(wPos);
        openvdb::Coord ijk((int)std::floor(iPos[0]),(int)std::floor(iPos[1]),(int)std::floor(iPos[2]));
        float d=acc.getValue(ijk);
        if(d>1e-6f){double se=d*ext;double ab=T*(1.0-std::exp(-se*step));
            float val=d;
            if(useTG){float tv=tAcc->getValue(ijk);
                val=(float)std::clamp(((double)tv-_tempMin)/(_tempMax-_tempMin),0.0,1.0);}
            wV+=val*ab;T*=std::exp(-se*step);}
    }
    double alpha=1.0-T;
    outDensity=(alpha>1e-6)?std::clamp((float)(wV/alpha),0.0f,1.0f):0.0f;
    outAlpha=(float)alpha;
}

// ═══════════════════════════════════════════════════════════════════════════════

static Op*build(Node*node){return new VDBRenderIop(node);}
const Op::Description VDBRenderIop::desc(VDBRenderIop::CLASS,build);
