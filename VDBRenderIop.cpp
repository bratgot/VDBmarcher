// VDBRender — OpenVDB Volume Ray Marcher for Nuke 17
// Created by Marten Blumen

#include "VDBRenderIop.h"
#include <DDImage/Knob.h>
#include <DDImage/Format.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace DD::Image;

const char* VDBRenderIop::CLASS = "VDBRender";
const char* VDBRenderIop::HELP =
    "VDBRender — OpenVDB Volume Ray Marcher\n\n"
    "Single and multiple scatter volume renderer\n"
    "with deep output, environment lighting,\n"
    "and Henyey-Greenstein phase function.\n\n"
    "Inputs:\n"
    "  bg  (0) — Background plate (sets resolution)\n"
    "  cam (1) — Camera\n"
    "  scn (2) — Scene (lights, axis, transforms)\n"
    "  env (3) — Environment HDRI (optional)\n\n"
    "Pipe lights and an Axis into a Scene node,\n"
    "then connect the Scene to the scn input.\n"
    "All lights in the tree are auto-detected.\n\n"
    "Created by Marten Blumen\n"
    "github.com/bratgot/VDBmarcher";

bool VDBRenderIop::_vdbInitialised = false;

// ═══ Blackbody / Ramps ═══

VDBRenderIop::Color3 VDBRenderIop::blackbody(double T) {
    if(T<100)T=100; double t=T/100, r,g,b;
    if(t<=66)r=255; else{r=329.698727446*std::pow(t-60,-0.1332047592);r=std::clamp(r,0.0,255.0);}
    if(t<=66){g=99.4708025861*std::log(t)-161.1195681661;g=std::clamp(g,0.0,255.0);}
    else{g=288.1221695283*std::pow(t-60,-0.0755148492);g=std::clamp(g,0.0,255.0);}
    if(t>=66)b=255; else if(t<=19)b=0;
    else{b=138.5177312231*std::log(t-10)-305.0447927307;b=std::clamp(b,0.0,255.0);}
    double bri=std::clamp(std::pow(T/6500,0.4),0.0,2.0);
    return{float(r/255*bri),float(g/255*bri),float(b/255*bri)};
}

VDBRenderIop::Color3 VDBRenderIop::evalRamp(ColorScheme s,float t,const float*gA,const float*gB,double tMin,double tMax) {
    t=std::clamp(t,0.0f,1.0f);
    switch(s){
    default:case kLit:case kGreyscale:return{t,t,t};
    case kExplosion:case kHeat:
        if(t<0.33f){float s2=t/0.33f;return{s2,0,0};}
        else if(t<0.66f){float s2=(t-0.33f)/0.33f;return{1,s2,0};}
        else{float s2=(t-0.66f)/0.34f;return{1,1,s2};}
    case kCool:
        if(t<0.33f){float s2=t/0.33f;return{0,0,s2};}
        else if(t<0.66f){float s2=(t-0.33f)/0.33f;return{0,s2,1};}
        else{float s2=(t-0.66f)/0.34f;return{s2,1,1};}
    case kBlackbody:return blackbody(tMin+t*(tMax-tMin));
    case kCustomGradient:return{gA[0]+t*(gB[0]-gA[0]),gA[1]+t*(gB[1]-gA[1]),gA[2]+t*(gB[2]-gA[2])};
    }
}

// ═══ Construction ═══

VDBRenderIop::VDBRenderIop(Node*node):Iop(node) {
    for(int c=0;c<3;++c)for(int r=0;r<3;++r)_camRot[c][r]=(c==r)?1:0;
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)_volFwd[i][j]=_volInv[i][j]=(i==j)?1:0;
    std::memset(_cachedVolFwd,0,sizeof(_cachedVolFwd));
    if(!_vdbInitialised){openvdb::initialize();_vdbInitialised=true;}
}

// ═══ Knobs ═══

void VDBRenderIop::knobs(Knob_Callback f)
{
    Text_knob(f,
        "<font size='+2'><b>VDBRender</b></font>"
        " <font color='#888' size='-1'>v1.0</font><br>"
        "<font color='#aaa'>OpenVDB volume ray marcher with multi-scatter,<br>"
        "deep output, and environment lighting</font>");
    Divider(f,"");

    Format_knob(f,&_formats,"format","Output Format");
    Tooltip(f,"Used when no BG plate is connected.\nBG input overrides this resolution.");

    Divider(f,"");
    File_knob(f,&_vdbFilePath,"file","VDB File");
    Tooltip(f,"Path to .vdb file. Sequences auto-detected\nfrom filename. Also supports #### and %04d.");
    Button(f,"discover_grids","Discover Grids");
    Tooltip(f,"Scans the VDB file and auto-populates\ngrid fields below with matching names.");

    Divider(f,"Grid Assignment");
    String_knob(f,&_gridName,"grid_name","Density");
    Tooltip(f,"Float grid for smoke opacity and scatter.\nCommon names: density, smoke, soot.\nLeave empty for emission-only rendering.");
    Double_knob(f,&_densityMix,"density_mix","Density Mix");
    SetRange(f,0,5);
    Tooltip(f,"Scales density grid values before shading.\n0 = no smoke. 1 = original. 2 = twice as dense.\nAffects opacity, scatter, and shadows.");
    String_knob(f,&_tempGridName,"temp_grid","Temperature");
    Tooltip(f,"Float grid for blackbody emission colour.\nCommon names: temperature, heat, temp.\nDrives the colour of self-luminous fire.");
    Double_knob(f,&_tempMix,"temp_mix","Temp Mix");
    SetRange(f,0,5);
    Tooltip(f,"Scales temperature values before emission.\n0 = no glow. 1 = original. >1 = brighter fire.");
    String_knob(f,&_flameGridName,"flame_grid","Flames");
    Tooltip(f,"Float grid for additional flame emission.\nCommon names: flame, flames, fire, fuel, burn.\nAdds extra glow on top of temperature.");
    Double_knob(f,&_flameMix,"flame_mix","Flame Mix");
    SetRange(f,0,5);
    Tooltip(f,"Scales flame values before emission.\n0 = no flames. 1 = original. >1 = brighter.");
    Int_knob(f,&_frameOffset,"frame_offset","Frame Offset");
    Tooltip(f,"Added to timeline frame for sequences.\nUse negative values to shift earlier.");

    Divider(f,"");
    static const char*presets[]={"Custom","Thin Smoke","Dense Smoke","Fog / Mist",
        "Cumulus Cloud","Fire","Explosion","Pyroclastic","Dust Storm","Steam",nullptr};
    Enumeration_knob(f,&_scenePreset,presets,"scene_preset","Scene Preset");
    Tooltip(f,"One-click setup for common volume types.\nSets render mode, density, scattering,\nanisotropy, shadows, emission, and quality.\nSwitch to Custom to tweak individual values.");

    static const char*modes[]={"Lit","Greyscale","Heat","Cool","Blackbody","Custom Gradient","Explosion",nullptr};
    Enumeration_knob(f,&_colorScheme,modes,"color_scheme","Render Mode");
    Tooltip(f,
        "Lit — physically-based scatter with shadows\n"
        "Greyscale — density mapped to luminance\n"
        "Heat — black to red to yellow to white\n"
        "Cool — black to blue to cyan to white\n"
        "Blackbody — Planckian temperature colour\n"
        "Custom Gradient — user two-colour ramp\n"
        "Explosion — lit smoke + self-luminous fire");
    Double_knob(f,&_rampIntensity,"ramp_intensity","Intensity");
    SetRange(f,0,10);
    Tooltip(f,"Master brightness for all render modes.\nMultiplies final RGB output. Does not\naffect alpha.");

    BeginGroup(f,"grp_density","Volume Density");
    Text_knob(f,
        "<font size='-1' color='#999'>"
        "Controls how the density grid is shaded. Extinction<br>"
        "sets how opaque the volume is. Scattering sets how<br>"
        "bright it appears under lighting. Anisotropy controls<br>"
        "whether light scatters forward or backward."
        "</font>");
    Double_knob(f,&_extinction,"extinction","Extinction");
    SetRange(f,0,100);
    Tooltip(f,"Light absorption per unit density.\nHigher = more opaque.\nThin smoke: 1-5. Cloud: 10-30. Solid: 50+.");
    Double_knob(f,&_scattering,"scattering","Scattering");
    SetRange(f,0,100);
    Tooltip(f,"In-scatter brightness under lighting.\nOnly affects Lit and Explosion modes.\nHigher = brighter. 0 = pure absorption.");
    Divider(f,"Phase Function");
    static const char*aniP[]={"Custom","Isotropic (0.0)","Smoke (0.4)","Dust (0.6)","Cloud (0.76)","Fog (0.8)","Rim Light (-0.4)","Strong Back (-0.7)",nullptr};
    Enumeration_knob(f,&_anisotropyPreset,aniP,"aniso_preset","Preset");
    Tooltip(f,"Presets for Henyey-Greenstein g value.\nSets the slider below.");
    Double_knob(f,&_anisotropy,"anisotropy","Anisotropy (g)");
    SetRange(f,-1,1);
    Tooltip(f,"Henyey-Greenstein phase function.\n-1 = strong back-scatter (rim light)\n 0 = isotropic (uniform)\n+1 = strong forward-scatter (backlit glow)\nSmoke: 0.4. Cloud: 0.76. Dust: 0.6.");
    EndGroup(f);

    BeginGroup(f,"grp_emission","Emission & Colour");
    Text_knob(f,
        "<font size='-1' color='#999'>"
        "Controls for temperature and flame emission.<br>"
        "Used by Blackbody, Explosion, and Lit modes<br>"
        "when temperature or flames grids are loaded.<br>"
        "Custom Gradient colours are set here too."
        "</font>");
    Divider(f,"Blackbody");
    Double_knob(f,&_tempMin,"temp_min","Temp Min (K)");
    SetRange(f,0,15000);
    Tooltip(f,"Kelvin at zero intensity.\nCandle: 1800. Fire base: 500.");
    Double_knob(f,&_tempMax,"temp_max","Temp Max (K)");
    SetRange(f,0,15000);
    Tooltip(f,"Kelvin at peak intensity.\nFire: 3000. Explosion: 8000. Plasma: 15000.");
    Double_knob(f,&_emissionIntensity,"emission_intensity","Temp Emission");
    SetRange(f,0,10);
    Tooltip(f,"Brightness of temperature grid emission.\nOnly active when a temperature grid is\nloaded. Start with 1-5.");
    Double_knob(f,&_flameIntensity,"flame_intensity","Flame Emission");
    SetRange(f,0,20);
    Tooltip(f,"Brightness of flame grid emission.\nOnly active when a flames grid is loaded.\nStart with 2-10.");
    Divider(f,"Custom Gradient");
    Color_knob(f,_gradStart,"grad_start","Start Color");
    Color_knob(f,_gradEnd,"grad_end","End Color");
    EndGroup(f);

    BeginGroup(f,"grp_light","Lighting");
    Text_knob(f,
        "<font size='-1' color='#999'>"
        "Lights are gathered from the scn input. Pipe<br>"
        "any number of Lights and an Axis into a Scene<br>"
        "node, then connect the Scene here. All lights<br>"
        "in the tree are found automatically.<br><br>"
        "If no lights are found, the fallback below is<br>"
        "used. Connect an HDRI to the env input for<br>"
        "environment lighting."
        "</font>");
    Divider(f,"Fallback Light");
    XYZ_knob(f,_lightDir,"light_dir","Direction");
    Tooltip(f,"Direction toward the light source.\nUsed when no lights found in scene input.");
    Color_knob(f,_lightColor,"light_color","Color");
    Tooltip(f,"Fallback light colour.");
    Double_knob(f,&_lightIntensity,"light_intensity","Intensity");
    SetRange(f,0,50);
    Tooltip(f,"Fallback light brightness.");
    Divider(f,"Ambient");
    Double_knob(f,&_ambientIntensity,"ambient","Ambient");
    SetRange(f,0,5);
    Tooltip(f,"Omnidirectional fill light, no shadows.\nLifts the darkest areas of dense volumes.\nUseful for volumes with strong self-shadowing.");
    Divider(f,"Environment Map");
    Text_knob(f,
        "<font size='-1' color='#999'>"
        "Connect a latlong HDRI to the env input.<br>"
        "Samples the image from multiple directions<br>"
        "with shadow rays for realistic sky lighting."
        "</font>");
    Double_knob(f,&_envIntensity,"env_intensity","Env Intensity");
    SetRange(f,0,10);
    Tooltip(f,"Brightness of environment lighting.\n0 = disabled. 1 = match image. >1 = brighter.");
    Double_knob(f,&_envRotate,"env_rotate","Env Rotate");
    SetRange(f,0,360);
    Tooltip(f,"Rotates the environment map horizontally.\n0 and 360 = same orientation.\nAnimatable for spinning light setups.");
    Double_knob(f,&_envDiffuse,"env_diffuse","Env Diffuse");
    SetRange(f,0,1);
    Tooltip(f,"How diffuse the environment lighting appears.\n0 = sharp, directional highlights\n0.5 = soft, natural spread\n1 = fully diffuse, even wrap\nHigher values are slower to render.");
    EndGroup(f);

    BeginClosedGroup(f,"grp_quality","Quality");
    static const char*qP[]={"Custom","Draft","Preview","Production","Final","Ultra",nullptr};
    Enumeration_knob(f,&_qualityPreset,qP,"quality_preset","Quality Preset");
    Tooltip(f,"Sets all quality controls at once.\nDraft = fastest, rough look.\nPreview = quick turnaround.\nProduction = good for client review.\nFinal = full quality delivery.\nUltra = maximum fidelity, slowest.");
    Text_knob(f,
        "<font size='-1' color='#999'>"
        "Quality slider controls ray march resolution.<br>"
        "Higher = finer detail but slower. Shadow steps<br>"
        "control self-shadow smoothness. Multi-scatter<br>"
        "adds light bounces for realistic clouds and fog."
        "</font>");
    Double_knob(f,&_quality,"quality","Quality");
    SetRange(f,1,10);
    Tooltip(f,"Ray march resolution (logarithmic).\n1 = fast preview\n3 = medium quality\n5 = good quality\n7 = high quality\n10 = final render");
    Int_knob(f,&_shadowSteps,"shadow_steps","Shadow Steps");
    Tooltip(f,"Shadow ray samples per light.\n4-8 = preview. 16-32 = final.");
    Double_knob(f,&_shadowDensity,"shadow_density","Shadow Density");
    SetRange(f,0,5);
    Tooltip(f,"Multiplier on extinction for shadow rays.\n1 = physically correct.\n<1 = lighter shadows. >1 = darker.\n0 = no shadows (fully lit).");
    Divider(f,"Multiple Scattering");
    static const char*scP[]={"Off","Preview","Thin Volume","Cloud / Fog",
        "Dense Volume","Ultra",nullptr};
    Enumeration_knob(f,&_scatterPreset,scP,"scatter_preset","Preset");
    Tooltip(f,"Quick setup for multi-scatter quality.\nOff = single scatter only (fastest).\nPreview = 1 bounce, 6 rays.\nThin Volume = 1 bounce, 14 rays.\nCloud / Fog = 2 bounces, 14 rays.\nDense Volume = 3 bounces, 26 rays.\nUltra = 4 bounces, 26 rays.");
    Int_knob(f,&_multiBounces,"multi_bounces","Bounces");
    Tooltip(f,"Extra light bounces through the volume.\n0 = single scatter (fastest)\n1 = one bounce (good for clouds)\n2-3 = dense fog / thick volumes\n4 = maximum realism, slowest");
    Int_knob(f,&_bounceRays,"bounce_rays","Bounce Rays");
    Tooltip(f,"Directions sampled per bounce.\n6 = axis-aligned (fast)\n14 = adds diagonals (smoother)\n26 = full coverage (best quality)");
    Divider(f,"Deep Output");
    Int_knob(f,&_deepSamples,"deep_samples","Deep Samples");
    Tooltip(f,"Max depth slabs per pixel for deep output.\nConnect to DeepMerge, DeepHoldout, or\nDeepWrite for deep compositing.\n16 = fast. 64 = smooth. 128 = final.");
    EndGroup(f);

    BeginClosedGroup(f,"grp_viewport","3D Viewport");
    Text_knob(f,
        "<font size='-1' color='#999'>"
        "Preview the volume bounding box and density<br>"
        "as a point cloud in the 3D viewer. Useful for<br>"
        "positioning the camera before rendering."
        "</font>");
    Bool_knob(f,&_showBbox,"show_bbox","Bounding Box");
    Tooltip(f,"Green wireframe around the volume.\nFollows Axis transform from scene input.");
    Bool_knob(f,&_showPoints,"show_points","Point Cloud");
    Tooltip(f,"Coloured dots representing density values.\nUpdates when the volume or transform changes.");
    static const char*dL[]={"Low (~16k)","Medium (~64k)","High (~250k)",nullptr};
    Enumeration_knob(f,&_pointDensity,dL,"point_density","Density");
    Tooltip(f,"Number of sample points for the preview.");
    Double_knob(f,&_pointSize,"point_size","Size");
    SetRange(f,1,20);
    Tooltip(f,"GL point size in pixels.");
    Divider(f,"Colour");
    Bool_knob(f,&_linkViewport,"link_viewport","Link to Render Mode");
    Tooltip(f,"Viewport colour scheme follows render mode.\nLit and Explosion fall back to Heat ramp.");
    static const char*vp[]={"Greyscale","Heat","Cool","Blackbody","Custom Gradient","Explosion",nullptr};
    Enumeration_knob(f,&_viewportColor,vp,"viewport_color","Manual");
    Tooltip(f,"Override the viewport colour scheme.");
    EndGroup(f);

    BeginClosedGroup(f,"grp_tech","Technical Reference");
    Text_knob(f,
        "<b>Radiative Transfer</b><br>"
        "<font size='-1' color='#bbb'>"
        "Emission-absorption volume rendering equation<br>"
        "evaluated via fixed-step ray marching. Each step<br>"
        "accumulates scatter, emission, and extinction.<br>"
        "Transmittance follows Beer-Lambert: T = exp(-ext)."
        "</font><br><br>"
        "<b>Phase Function</b><br>"
        "<font size='-1' color='#bbb'>"
        "Henyey-Greenstein angular scatter distribution.<br>"
        "g=0 isotropic, g>0 forward scatter (backlit glow),<br>"
        "g&lt;0 back scatter (rim light / halo effect)."
        "</font><br><br>"
        "<b>Multiple Scattering</b><br>"
        "<font size='-1' color='#bbb'>"
        "N-bounce approximation. At each sample, light is<br>"
        "scattered in 6-26 directions, each with abbreviated<br>"
        "shadow rays. Energy conserved via albedo weighting.<br>"
        "Essential for realistic clouds and thick fog."
        "</font><br><br>"
        "<b>Blackbody Emission</b><br>"
        "<font size='-1' color='#bbb'>"
        "Planckian locus maps Kelvin to CIE 1931 colour.<br>"
        "1000K deep red, 2500K orange, 6500K daylight,<br>"
        "10000K blue-white plasma."
        "</font><br><br>"
        "<b>Explosion Mode</b><br>"
        "<font size='-1' color='#bbb'>"
        "Combines density-driven scatter with shadows<br>"
        "(smoke) and self-luminous blackbody emission<br>"
        "(fire). Fire glows independently of scene lights."
        "</font><br><br>"
        "<b>Environment Lighting</b><br>"
        "<font size='-1' color='#bbb'>"
        "Latlong HDRI sampled from multiple directions<br>"
        "with shadow rays. Respects volume transform."
        "</font><br><br>"
        "<b>Deep Output</b><br>"
        "<font size='-1' color='#bbb'>"
        "Outputs depth-sorted RGBA slabs for deep comp.<br>"
        "Connect to DeepMerge for depth-correct integration<br>"
        "with CG renders. Pure emission gets synthesised alpha."
        "</font><br><br>"
        "<b>Scene Input</b><br>"
        "<font size='-1' color='#bbb'>"
        "Recursively walks the scene tree to find lights<br>"
        "and axis transforms. Connect any number of lights<br>"
        "into a Scene node, pipe to scn input."
        "</font><br><br>"
        "<b>OpenVDB Grids</b><br>"
        "<font size='-1' color='#bbb'>"
        "density — scatter/absorption (float)<br>"
        "temperature — emission Kelvin (float)<br>"
        "heat — normalised 0-1 (float)<br>"
        "flame/flames — combustion (float)<br>"
        "vel/v — velocity, not yet used (vec3)"
        "</font><br><br>"
        "<font size='-1' color='#888'>"
        "Museth (2013) ACM TOG 32(3)<br>"
        "Henyey/Greenstein (1941) ApJ 93<br>"
        "Fong+ (2017) SIGGRAPH Course<br>"
        "Novak+ (2018) CGF 37(2)"
        "</font>");
    EndGroup(f);

    Divider(f,"");
    Text_knob(f,
        "<font size='-1' color='#666'>"
        "<b>Created by Marten Blumen</b><br>"
        "github.com/bratgot/VDBmarcher"
        "</font><br>"
        "<font size='-1' color='#555'>"
        "OpenVDB 12 · C++20 · Nuke 17<br>"
        "Beer-Lambert · Henyey-Greenstein · Planckian"
        "</font>");
}

int VDBRenderIop::knob_changed(Knob* k)
{
    if(k->is("file")){_gridValid=false;_previewPoints.clear();return 1;}
    if(k->is("grid_name")||k->is("temp_grid")||k->is("flame_grid")||k->is("frame_offset")){
        _gridValid=false;_previewPoints.clear();return 1;}
    if(k->is("show_points")||k->is("point_density")){_previewPoints.clear();return 1;}
    if(k->is("discover_grids")){discoverGrids();return 1;}
    if(k->is("scene_preset")&&_scenePreset>0){
        // Preset values: mode, extinction, scattering, anisotropy, shadowSteps,
        //                shadowDensity, tempMin, tempMax, emissionInt, flameInt, stepSize
        struct Preset {
            int mode; double ext,scat,aniso; int shSteps;
            double shDen,tMin,tMax,emInt,flInt,quality,ambient;
            int bounces,bRays; double intensity,envDiff;
        };
        //                                   mode ext   scat  ani  sh shDen tMn   tMx    eI   fI   q    amb  bnc bR   int  envD
        static const Preset pv[] = {
            {},                             // 0: Custom
            {0, 2.0, 1.5, 0.4, 8, 1.0,       500,6500,  0,   0,   2, 0.1,  0, 6,  1.0, 0.5},  // 1: Thin Smoke
            {0, 15.0,4.0, 0.35,12,1.0,       500,6500,  0,   0,   3, 0.05, 1, 6,  1.0, 0.5},  // 2: Dense Smoke
            {0, 0.5, 0.8, 0.8, 8, 0.5,       500,6500,  0,   0,   1, 0.3,  2, 14, 1.0, 0.8},  // 3: Fog / Mist
            {0, 12.0,10.0,0.76,16,1.0,       500,6500,  0,   0,   5, 0.2,  2, 14, 1.0, 0.6},  // 4: Cumulus Cloud
            {6, 5.0, 2.0, 0.3, 8, 0.6,       800,3000,  4.0, 8.0, 3, 0.15, 0, 6,  1.5, 0.3},  // 5: Fire
            {6, 20.0,5.0, 0.4, 16,0.5,       500,6000,  3.0, 5.0, 5, 0.1,  1, 14, 1.0, 0.4},  // 6: Explosion
            {6, 30.0,6.0, 0.5, 16,0.4,      1000,8000,  2.0, 4.0, 7, 0.08, 1, 14, 0.8, 0.3},  // 7: Pyroclastic
            {0, 4.0, 3.0,-0.3, 8, 1.0,       500,6500,  0,   0,   2, 0.15, 0, 6,  1.0, 0.5},  // 8: Dust Storm
            {0, 1.5, 2.0, 0.7, 8, 0.5,       500,6500,  0,   0,   2, 0.2,  1, 6,  1.0, 0.7},  // 9: Steam
        };
        const auto&p=pv[_scenePreset];
        knob("color_scheme")->set_value(p.mode);_colorScheme=p.mode;
        knob("extinction")->set_value(p.ext);_extinction=p.ext;
        knob("scattering")->set_value(p.scat);_scattering=p.scat;
        knob("anisotropy")->set_value(p.aniso);_anisotropy=p.aniso;
        knob("shadow_steps")->set_value(p.shSteps);_shadowSteps=p.shSteps;
        knob("shadow_density")->set_value(p.shDen);_shadowDensity=p.shDen;
        knob("temp_min")->set_value(p.tMin);_tempMin=p.tMin;
        knob("temp_max")->set_value(p.tMax);_tempMax=p.tMax;
        knob("emission_intensity")->set_value(p.emInt);_emissionIntensity=p.emInt;
        knob("flame_intensity")->set_value(p.flInt);_flameIntensity=p.flInt;
        knob("quality")->set_value(p.quality);_quality=p.quality;
        knob("ambient")->set_value(p.ambient);_ambientIntensity=p.ambient;
        knob("multi_bounces")->set_value(p.bounces);_multiBounces=p.bounces;
        knob("bounce_rays")->set_value(p.bRays);_bounceRays=p.bRays;
        knob("ramp_intensity")->set_value(p.intensity);_rampIntensity=p.intensity;
        knob("env_diffuse")->set_value(p.envDiff);_envDiffuse=p.envDiff;
        knob("aniso_preset")->set_value(0);_anisotropyPreset=0;
        knob("scatter_preset")->set_value(0);_scatterPreset=0;
        knob("quality_preset")->set_value(0);_qualityPreset=0;
        return 1;
    }
    if(k->is("aniso_preset")){
        static const double pv[]={0,0,0.4,0.6,0.76,0.8,-0.4,-0.7};
        if(_anisotropyPreset>0&&_anisotropyPreset<(int)(sizeof(pv)/sizeof(double))){
            _anisotropy=pv[_anisotropyPreset];knob("anisotropy")->set_value(_anisotropy);}
        return 1;}
    if(k->is("scatter_preset")){
        static const int bounces[]={0,1,1,2,3,4};
        static const int rays[]   ={6,6,14,14,26,26};
        if(_scatterPreset<(int)(sizeof(bounces)/sizeof(int))){
            knob("multi_bounces")->set_value(bounces[_scatterPreset]);
            _multiBounces=bounces[_scatterPreset];
            knob("bounce_rays")->set_value(rays[_scatterPreset]);
            _bounceRays=rays[_scatterPreset];}
        return 1;}
    if(k->is("quality_preset")){
        // Custom(0), Draft(1), Preview(2), Production(3), Final(4), Ultra(5)
        struct QPreset { double q; int sh; double shDen; int bnc,bRays,deep; double envD; };
        static const QPreset qv[]={
            {},                               // 0: Custom
            {1.0,  4, 0.5, 0,  6,  16, 0.3}, // 1: Draft
            {2.0,  8, 1.0, 0,  6,  16, 0.4}, // 2: Preview
            {5.0, 16, 1.0, 1, 14,  32, 0.6}, // 3: Production
            {7.0, 24, 1.0, 2, 14,  64, 0.7}, // 4: Final
            {10.0,32, 1.0, 3, 26, 128, 1.0}, // 5: Ultra
        };
        if(_qualityPreset>0&&_qualityPreset<(int)(sizeof(qv)/sizeof(QPreset))){
            const auto&q=qv[_qualityPreset];
            knob("quality")->set_value(q.q);_quality=q.q;
            knob("shadow_steps")->set_value(q.sh);_shadowSteps=q.sh;
            knob("shadow_density")->set_value(q.shDen);_shadowDensity=q.shDen;
            knob("multi_bounces")->set_value(q.bnc);_multiBounces=q.bnc;
            knob("bounce_rays")->set_value(q.bRays);_bounceRays=q.bRays;
            knob("deep_samples")->set_value(q.deep);_deepSamples=q.deep;
            knob("env_diffuse")->set_value(q.envD);_envDiffuse=q.envD;
            knob("scatter_preset")->set_value(0);_scatterPreset=0;}
        return 1;}
    return Iop::knob_changed(k);
}

// ═══ Inputs ═══

CameraOp* VDBRenderIop::camera() const{return dynamic_cast<CameraOp*>(Op::input(1));}

const char* VDBRenderIop::input_label(int idx,char*buf) const{
    if(idx==0)return"bg";
    if(idx==1)return"cam";
    if(idx==2)return"scn";
    if(idx==3)return"env";
    return nullptr;}

bool VDBRenderIop::test_input(int idx,Op*op) const{
    if(idx==0)return dynamic_cast<Iop*>(op)!=nullptr;
    if(idx==1)return dynamic_cast<CameraOp*>(op)||Iop::test_input(idx,op);
    if(idx==2)return true; // Scene, Axis, Light, TransformGeo — anything 3D
    if(idx==3)return dynamic_cast<Iop*>(op)!=nullptr;
    return Iop::test_input(idx,op);}

Op* VDBRenderIop::default_input(int idx) const{
    if(idx==0)return Iop::default_input(idx);
    return nullptr;}

// Recursively walk the scene input tree to find LightOps and AxisOps
void VDBRenderIop::gatherLights(Op* scnOp) {
    if(!scnOp) return;
    scnOp->validate(true);

    // Check if this op is a Light
    LightOp* light = dynamic_cast<LightOp*>(scnOp);
    if(light) {
        const auto lm = light->worldTransform();
        CachedLight cl;
        cl.pos[0]=(double)lm[3][0]; cl.pos[1]=(double)lm[3][1]; cl.pos[2]=(double)lm[3][2];
        cl.dir[0]=(double)lm[2][0]; cl.dir[1]=(double)lm[2][1]; cl.dir[2]=(double)lm[2][2];
        double len=std::sqrt(cl.dir[0]*cl.dir[0]+cl.dir[1]*cl.dir[1]+cl.dir[2]*cl.dir[2]);
        if(len>1e-8){cl.dir[0]/=len;cl.dir[1]/=len;cl.dir[2]/=len;}
        cl.isPoint=true;
        double lr=1,lg=1,lb=1,intensity=1;
        if(Knob*ck=light->knob("color")){
            lr=ck->get_value_at(outputContext().frame(),0);
            lg=ck->get_value_at(outputContext().frame(),1);
            lb=ck->get_value_at(outputContext().frame(),2);}
        if(Knob*ik=light->knob("intensity"))
            intensity=ik->get_value_at(outputContext().frame());
        cl.color[0]=lr*intensity;cl.color[1]=lg*intensity;cl.color[2]=lb*intensity;
        _lights.push_back(cl);
    }

    // Check if this is a non-light AxisOp (volume transform)
    if(!light && !_hasVolumeXform) {
        AxisOp* axis = dynamic_cast<AxisOp*>(scnOp);
        if(axis && !dynamic_cast<CameraOp*>(scnOp)) {
            const auto axM = axis->worldTransform();
            for(int c=0;c<4;++c)for(int r=0;r<4;++r)_volFwd[c][r]=(double)axM[c][r];
            Matrix4 m4;
            for(int c=0;c<4;++c)for(int r=0;r<4;++r)m4[c][r]=(float)_volFwd[c][r];
            auto inv=m4.inverse();
            for(int c=0;c<4;++c)for(int r=0;r<4;++r)_volInv[c][r]=(double)inv[c][r];
            _hasVolumeXform=true;
        }
    }

    // Recurse into this op's inputs (Scene merges its inputs)
    for(int i=0; i<scnOp->inputs(); ++i) {
        Op* child = scnOp->input(i);
        if(child && child != scnOp) gatherLights(child);
    }
}

// ═══ Frame Sequence ═══

std::string VDBRenderIop::resolveFramePath(int frame) const {
    std::string p(_vdbFilePath?_vdbFilePath:"");if(p.empty())return p;
    size_t h=p.find('#');
    if(h!=std::string::npos){size_t he=h;while(he<p.size()&&p[he]=='#')++he;
        char b[64];std::snprintf(b,64,"%0*d",(int)(he-h),frame);p.replace(h,he-h,b);return p;}
    size_t pc=p.find('%');
    if(pc!=std::string::npos){size_t s=pc+1;if(s<p.size()&&p[s]=='0')++s;
        while(s<p.size()&&p[s]>='0'&&p[s]<='9')++s;
        if(s<p.size()&&p[s]=='d'){char b[64];std::snprintf(b,64,p.substr(pc,s-pc+1).c_str(),frame);
            p.replace(pc,s-pc+1,b);return p;}}
    size_t dot=p.rfind('.');if(dot==std::string::npos)dot=p.size();
    size_t ne=dot,ns=ne;while(ns>0&&p[ns-1]>='0'&&p[ns-1]<='9')--ns;
    if(ns<ne){char b[64];std::snprintf(b,64,"%0*d",(int)(ne-ns),frame);p.replace(ns,ne-ns,b);return p;}
    return p;
}

// ═══ Discover Grids ═══

void VDBRenderIop::discoverGrids() {
    std::string path(_vdbFilePath?_vdbFilePath:"");
    if(path.empty()){error("No VDB file.");return;}
    int cf=(int)outputContext().frame()+_frameOffset;
    std::string resolved=resolveFramePath(cf);
    {std::string tp=resolved;for(auto&c:tp)if(c=='\\')c='/';
     FILE*f=fopen(tp.c_str(),"rb");
     if(!f){std::string orig(path);for(auto&c:orig)if(c=='\\')c='/';
         FILE*of=fopen(orig.c_str(),"rb");if(of){fclose(of);resolved=orig;}
         else{error("Cannot open file.");return;}}
     else fclose(f);}
    std::string cp=resolved;for(auto&c:cp)if(c=='\\')c='/';
    try{
        openvdb::io::File file(cp);file.open();
        static const char*denN[]={"density","density_1","smoke","soot","absorption","scatter",nullptr};
        static const char*tmpN[]={"temperature","heat","temp",nullptr};
        static const char*flmN[]={"flame","flames","fire","fuel","burn","incandescence","emission",nullptr};
        auto match=[](const std::string&n,const char**l){for(int i=0;l[i];++i)if(n==l[i])return true;return false;};
        struct GI{std::string name,type,cat;};
        std::vector<GI> grids;std::string bestD,bestT,bestF;
        for(auto it=file.beginName();it!=file.endName();++it){
            std::string n=it.gridName();auto g=file.readGridMetadata(n);std::string ty=g->valueType();
            std::string cat="other";
            if(match(n,denN))cat="density";else if(match(n,tmpN))cat="temperature";else if(match(n,flmN))cat="flames";
            grids.push_back({n,ty,cat});
            if(bestD.empty()&&cat=="density"&&ty=="float")bestD=n;
            if(bestT.empty()&&cat=="temperature"&&ty=="float")bestT=n;
            if(bestF.empty()&&cat=="flames"&&ty=="float")bestF=n;
        }
        file.close();
        if(!bestD.empty())knob("grid_name")->set_text(bestD.c_str());
        if(!bestT.empty())knob("temp_grid")->set_text(bestT.c_str());
        if(!bestF.empty())knob("flame_grid")->set_text(bestF.c_str());
        // Auto-set mode
        if(!bestT.empty()||!bestF.empty()){knob("color_scheme")->set_value(6);knob("emission_intensity")->set_value(2.0);}
        std::string msg;
        for(const auto&g:grids){msg+=g.name+" ("+g.type+")";if(g.cat!="other")msg+=" ["+g.cat+"]";msg+="\\n";}
        msg+="\\n";
        if(!bestD.empty())msg+="Density: "+bestD+"\\n";
        if(!bestT.empty())msg+="Temperature: "+bestT+"\\n";
        if(!bestF.empty())msg+="Flames: "+bestF+"\\n";
        if(!bestT.empty()||!bestF.empty())msg+="\\nRender mode set to Explosion.";
        script_command(("nuke.message('"+msg+"')").c_str());
        _gridValid=false;_previewPoints.clear();
    }catch(const std::exception&e){error("Discover: %s",e.what());}
}

// ═══ Hash ═══

void VDBRenderIop::append(Hash& hash) {
    hash.append(outputContext().frame());hash.append(_frameOffset);hash.append(_colorScheme);
    hash.append(_emissionIntensity);hash.append(_flameIntensity);hash.append(_rampIntensity);hash.append(_lightIntensity);
    hash.append(_anisotropy);hash.append(_shadowDensity);hash.append(_ambientIntensity);
    hash.append(_multiBounces);hash.append(_bounceRays);
    hash.append(_envIntensity);hash.append(_envDiffuse);hash.append(_envRotate);
    hash.append(_densityMix);hash.append(_tempMix);hash.append(_flameMix);
    for(int i=0;i<3;++i){hash.append(_lightDir[i]);hash.append(_lightColor[i]);}
    // Hash cam, scene tree, and env inputs
    if(input(1))hash.append(Op::input(1)->hash());
    if(input(2))hash.append(Op::input(2)->hash());
    if(inputs()>3&&input(3))hash.append(Op::input(3)->hash());
}

// ═══ 3D Viewport ═══

void VDBRenderIop::build_handles(ViewerContext*ctx){
    if(!_gridValid||(!_showBbox&&!_showPoints))return;add_draw_handle(ctx);}

void VDBRenderIop::rebuildPointCloud() {
    _previewPoints.clear();_maxDensity=1;if(!_floatGrid)return;
    int res=24;if(_pointDensity==1)res=40;if(_pointDensity==2)res=64;
    auto acc=_floatGrid->getConstAccessor();const auto&xf=_floatGrid->transform();
    double dx=(_bboxMax[0]-_bboxMin[0])/res,dy=(_bboxMax[1]-_bboxMin[1])/res,dz=(_bboxMax[2]-_bboxMin[2])/res;
    _previewPoints.reserve(res*res*res/4);float maxD=0;
    for(int iz=0;iz<res;++iz){double wz=_bboxMin[2]+(iz+.5)*dz;
    for(int iy=0;iy<res;++iy){double wy=_bboxMin[1]+(iy+.5)*dy;
    for(int ix=0;ix<res;++ix){double wx=_bboxMin[0]+(ix+.5)*dx;
        auto ip=xf.worldToIndex(openvdb::Vec3d(wx,wy,wz));
        float d=acc.getValue(openvdb::Coord((int)std::floor(ip[0]),(int)std::floor(ip[1]),(int)std::floor(ip[2])));
        if(d>.001f){float px=(float)wx,py=(float)wy,pz=(float)wz;
            if(_hasVolumeXform){px=(float)(_volFwd[0][0]*wx+_volFwd[1][0]*wy+_volFwd[2][0]*wz+_volFwd[3][0]);
                py=(float)(_volFwd[0][1]*wx+_volFwd[1][1]*wy+_volFwd[2][1]*wz+_volFwd[3][1]);
                pz=(float)(_volFwd[0][2]*wx+_volFwd[1][2]*wy+_volFwd[2][2]*wz+_volFwd[3][2]);}
            _previewPoints.push_back({px,py,pz,d});if(d>maxD)maxD=d;}
    }}}_maxDensity=(maxD>0)?maxD:1;
}

void VDBRenderIop::draw_handle(ViewerContext*ctx) {
    if(!_gridValid)return;
    glPushAttrib(GL_CURRENT_BIT|GL_LINE_BIT|GL_ENABLE_BIT|GL_POINT_BIT);glDisable(GL_LIGHTING);
    if(_showBbox){float co[8][3];int ci=0;
        for(int iz=0;iz<=1;++iz)for(int iy=0;iy<=1;++iy)for(int ix=0;ix<=1;++ix){
            double wx=ix?_bboxMax[0]:_bboxMin[0],wy=iy?_bboxMax[1]:_bboxMin[1],wz=iz?_bboxMax[2]:_bboxMin[2];
            if(_hasVolumeXform){double tx=_volFwd[0][0]*wx+_volFwd[1][0]*wy+_volFwd[2][0]*wz+_volFwd[3][0];
                double ty=_volFwd[0][1]*wx+_volFwd[1][1]*wy+_volFwd[2][1]*wz+_volFwd[3][1];
                double tz=_volFwd[0][2]*wx+_volFwd[1][2]*wy+_volFwd[2][2]*wz+_volFwd[3][2];
                wx=tx;wy=ty;wz=tz;}
            co[ci][0]=(float)wx;co[ci][1]=(float)wy;co[ci][2]=(float)wz;++ci;}
        glLineWidth(1.5f);glColor3f(0,1,0);glBegin(GL_LINES);
        for(int e=0;e<12;++e){glVertex3fv(co[_bboxEdges[e][0]]);glVertex3fv(co[_bboxEdges[e][1]]);}glEnd();
        float cx=0,cy=0,cz=0;for(int i=0;i<8;++i){cx+=co[i][0];cy+=co[i][1];cz+=co[i][2];}
        cx/=8;cy/=8;cz/=8;float sz=0;
        for(int i=0;i<8;++i){float d=std::sqrt((co[i][0]-cx)*(co[i][0]-cx)+(co[i][1]-cy)*(co[i][1]-cy)+(co[i][2]-cz)*(co[i][2]-cz));if(d>sz)sz=d;}
        sz*=.05f;glColor3f(1,1,0);glBegin(GL_LINES);
        glVertex3f(cx-sz,cy,cz);glVertex3f(cx+sz,cy,cz);glVertex3f(cx,cy-sz,cz);glVertex3f(cx,cy+sz,cz);
        glVertex3f(cx,cy,cz-sz);glVertex3f(cx,cy,cz+sz);glEnd();}
    if(_showPoints&&_floatGrid){
        int cf=(int)outputContext().frame()+_frameOffset;std::string cp=resolveFramePath(cf);
        if(_previewPoints.empty()||_cachedPointDensity!=_pointDensity||_cachedPointsPath!=cp
           ||_cachedPointsFrame!=cf||_cachedHasXform!=_hasVolumeXform
           ||std::memcmp(_cachedVolFwd,_volFwd,sizeof(_volFwd))!=0){
            rebuildPointCloud();_cachedPointDensity=_pointDensity;_cachedPointsPath=cp;
            _cachedPointsFrame=cf;_cachedHasXform=_hasVolumeXform;std::memcpy(_cachedVolFwd,_volFwd,sizeof(_volFwd));}
        if(!_previewPoints.empty()){
            ColorScheme vs;
            if(_linkViewport)vs=(_colorScheme==kLit)?kGreyscale:(_colorScheme==kExplosion)?kHeat:static_cast<ColorScheme>(_colorScheme);
            else switch(_viewportColor){default:case 0:vs=kGreyscale;break;case 1:vs=kHeat;break;case 2:vs=kCool;break;
                case 3:vs=kBlackbody;break;case 4:vs=kCustomGradient;break;case 5:vs=kHeat;break;}
            float gA[3]={(float)_gradStart[0],(float)_gradStart[1],(float)_gradStart[2]};
            float gB[3]={(float)_gradEnd[0],(float)_gradEnd[1],(float)_gradEnd[2]};
            float inv=1.f/_maxDensity;
            glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glPointSize((float)_pointSize);glEnable(GL_POINT_SMOOTH);
            glBegin(GL_POINTS);
            for(const auto&pt:_previewPoints){float t=std::min(pt.density*inv,1.f);
                Color3 c=evalRamp(vs,t,gA,gB,_tempMin,_tempMax);
                glColor4f(c.r,c.g,c.b,.15f+.85f*t);glVertex3f(pt.x,pt.y,pt.z);}
            glEnd();glDisable(GL_BLEND);}}
    glPopAttrib();
}

// ═══ _validate ═══

void VDBRenderIop::_validate(bool for_real) {
    _camValid=false;_hasVolumeXform=false;_lights.clear();
    // Validate cam and scene inputs
    if(input(1)) Op::input(1)->validate(for_real);
    if(input(2)) Op::input(2)->validate(for_real);
    // BG input 0 — if connected, use its format
    // BG input 0
    Iop* bgIop=dynamic_cast<Iop*>(Op::input(0));
    if(bgIop) bgIop->validate(for_real);
    // Use BG format if a real node is connected (not default black)
    bool bgConnected=bgIop && dynamic_cast<Iop*>(Op::input(0))!=Iop::default_input(0);
    if(bgConnected){
        const Format&bgFmt=bgIop->info().format();
        const Format&bgFull=bgIop->info().full_size_format();
        info_.format(bgFmt);info_.full_size_format(bgFull);info_.set(bgFmt);
    }else{
        const Format*fmt=_formats.format();const Format*full=_formats.fullSizeFormat();if(!full)full=fmt;
        if(fmt){info_.format(*fmt);info_.full_size_format(full?*full:*fmt);info_.set(*fmt);}
    }
    info_.channels(Mask_RGBA);info_.turn_on(Mask_RGBA);
    set_out_channels(Mask_RGBA);
    // Deep info — MUST be set on both validate passes
    {ChannelSet deepChans=Mask_RGBA;deepChans+=Chan_DeepFront;deepChans+=Chan_DeepBack;
     const Format&outFmt=info_.format();
     int w=outFmt.width(),h=outFmt.height();
     Box dbox(0,0,w,h);
     DeepOp::_deepInfo=DeepInfo(_formats,dbox,deepChans);}
    if(!for_real)return;

    // Camera
    if(CameraOp*cam=camera()){const auto cw=cam->worldTransform();
        _camOrigin=openvdb::Vec3d(cw[3][0],cw[3][1],cw[3][2]);
        for(int c=0;c<3;++c)for(int r=0;r<3;++r)_camRot[c][r]=(double)cw[c][r];
        _halfW=(double)cam->horizontalAperture()*.5/(double)cam->focalLength();_camValid=true;
    }else{return;}

    // Scene input (2): gather lights + axis from scene tree
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)_volFwd[i][j]=_volInv[i][j]=(i==j)?1:0;
    if(input(2)) gatherLights(Op::input(2));

    // Fallback light if none found in scene
    if(_lights.empty()){CachedLight cl;
        cl.dir[0]=_lightDir[0];cl.dir[1]=_lightDir[1];cl.dir[2]=_lightDir[2];
        double len=std::sqrt(cl.dir[0]*cl.dir[0]+cl.dir[1]*cl.dir[1]+cl.dir[2]*cl.dir[2]);
        if(len>1e-8){cl.dir[0]/=len;cl.dir[1]/=len;cl.dir[2]/=len;}
        cl.color[0]=_lightColor[0]*_lightIntensity;cl.color[1]=_lightColor[1]*_lightIntensity;cl.color[2]=_lightColor[2]*_lightIntensity;
        cl.isPoint=false;cl.pos[0]=cl.pos[1]=cl.pos[2]=0;_lights.push_back(cl);}

    // Transform lights into volume-local space
    if(_hasVolumeXform){for(auto&cl:_lights){
        double dx=_volInv[0][0]*cl.dir[0]+_volInv[1][0]*cl.dir[1]+_volInv[2][0]*cl.dir[2];
        double dy=_volInv[0][1]*cl.dir[0]+_volInv[1][1]*cl.dir[1]+_volInv[2][1]*cl.dir[2];
        double dz=_volInv[0][2]*cl.dir[0]+_volInv[1][2]*cl.dir[1]+_volInv[2][2]*cl.dir[2];
        double l=std::sqrt(dx*dx+dy*dy+dz*dz);if(l>1e-8){dx/=l;dy/=l;dz/=l;}
        cl.dir[0]=dx;cl.dir[1]=dy;cl.dir[2]=dz;
        if(cl.isPoint){double px=_volInv[0][0]*cl.pos[0]+_volInv[1][0]*cl.pos[1]+_volInv[2][0]*cl.pos[2]+_volInv[3][0];
            double py=_volInv[0][1]*cl.pos[0]+_volInv[1][1]*cl.pos[1]+_volInv[2][1]*cl.pos[2]+_volInv[3][1];
            double pz=_volInv[0][2]*cl.pos[0]+_volInv[1][2]*cl.pos[1]+_volInv[2][2]*cl.pos[2]+_volInv[3][2];
            cl.pos[0]=px;cl.pos[1]=py;cl.pos[2]=pz;}}}

    // Environment map (input 3) — only re-cache when input changes
    {Iop*eIop=(inputs()>3&&input(3))?dynamic_cast<Iop*>(Op::input(3)):nullptr;
     if(eIop&&_envIntensity>0){
         eIop->validate(true);
         // Only mark dirty if the env input changed or we never cached
         if(eIop!=_envIop||!_hasEnvMap){
             _envIop=eIop;_envDirty=true;
         }
     }else{
         _hasEnvMap=false;_envDirty=false;_envIop=nullptr;
     }
    }

    // Load VDB
    {Guard guard(_loadLock);
    int curFrame=(int)outputContext().frame()+_frameOffset;
    std::string path2=resolveFramePath(curFrame);std::string grid(_gridName?_gridName:"");
    {std::string tp=path2;for(auto&c:tp)if(c=='\\')c='/';FILE*f=fopen(tp.c_str(),"rb");
     if(!f){std::string orig(_vdbFilePath?_vdbFilePath:"");if(orig!=path2){for(auto&c:orig)if(c=='\\')c='/';
         FILE*of=fopen(orig.c_str(),"rb");if(of){fclose(of);path2=orig;}}}else fclose(f);}
    if(!_gridValid||path2!=_loadedPath||grid!=_loadedGrid||curFrame!=_loadedFrame){
        _floatGrid.reset();_tempGrid.reset();_flameGrid.reset();
        _gridValid=false;_hasTempGrid=false;_hasFlameGrid=false;_previewPoints.clear();
        if(!path2.empty()){try{
            std::string cp2=path2;for(auto&c:cp2)if(c=='\\')c='/';
            openvdb::io::File file(cp2);file.open();
            std::string target=grid;
            // Try to load density grid (optional if other grids exist)
            openvdb::GridBase::Ptr bg;bool found=false;
            if(!target.empty()){
                for(auto it=file.beginName();it!=file.endName();++it)
                    if(it.gridName()==target){bg=file.readGrid(target);found=true;break;}
            }
            // Temperature grid — only load if field is explicitly set
            std::string tgN(_tempGridName?_tempGridName:"");openvdb::GridBase::Ptr tbg;
            if(!tgN.empty()){for(auto it=file.beginName();it!=file.endName();++it)
                if(it.gridName()==tgN){tbg=file.readGrid(tgN);break;}}
            // Flames grid — only load if field is explicitly set
            std::string fgN(_flameGridName?_flameGridName:"");openvdb::GridBase::Ptr fbg;
            if(!fgN.empty()){for(auto it=file.beginName();it!=file.endName();++it)
                if(it.gridName()==fgN){fbg=file.readGrid(fgN);break;}}
            file.close();
            // Need at least one grid
            bool hasDensity=found&&bg&&bg->isType<openvdb::FloatGrid>();
            bool hasTemp=tbg&&tbg->isType<openvdb::FloatGrid>();
            bool hasFlame=fbg&&fbg->isType<openvdb::FloatGrid>();
            if(!hasDensity&&!hasTemp&&!hasFlame){error("No valid grids found. Set grid names or use Discover Grids.");return;}
            if(hasDensity) _floatGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(bg);
            if(hasTemp){_tempGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(tbg);_hasTempGrid=true;}
            if(hasFlame){_flameGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(fbg);_hasFlameGrid=true;}
            // Compute bbox from whichever grid we have (prefer density, fallback to temp/flame)
            openvdb::FloatGrid::Ptr bboxGrid=_floatGrid?_floatGrid:(_tempGrid?_tempGrid:_flameGrid);
            auto ab=bboxGrid->evalActiveVoxelBoundingBox();if(ab.empty()){error("No active voxels.");return;}
            const auto&xf=bboxGrid->transform();const auto&lo=ab.min();const auto&hi=ab.max();
            openvdb::Vec3d corners[8];int ci2=0;
            for(int iz=0;iz<=1;++iz)for(int iy=0;iy<=1;++iy)for(int ix=0;ix<=1;++ix)
                corners[ci2++]=xf.indexToWorld(openvdb::Vec3d(ix?hi.x()+1.:lo.x(),iy?hi.y()+1.:lo.y(),iz?hi.z()+1.:lo.z()));
            _bboxMin=_bboxMax=corners[0];
            for(int i=1;i<8;++i)for(int a=0;a<3;++a){_bboxMin[a]=std::min(_bboxMin[a],corners[i][a]);_bboxMax[a]=std::max(_bboxMax[a],corners[i][a]);}
            _gridValid=true;_loadedPath=path2;_loadedGrid=grid;_loadedFrame=curFrame;
            // Build VolumeRayIntersector for HDDA empty-space skipping
            _volRI.reset();
            openvdb::FloatGrid::Ptr riGrid=_floatGrid?_floatGrid:(_tempGrid?_tempGrid:_flameGrid);
            if(riGrid){try{_volRI=std::make_unique<VRI>(*riGrid);}catch(...){}}
        }catch(const std::exception&e){error("OpenVDB: %s",e.what());}catch(...){error("OpenVDB error.");}}
    }}
}

// ═══ engine ═══

void VDBRenderIop::_request(int x,int y,int r,int t,ChannelMask channels,int count){
    Iop*bg=dynamic_cast<Iop*>(Op::input(0));
    if(bg) bg->request(x,y,r,t,channels,count);
    // Request full env image for caching
    if(_envIop&&_envDirty){
        const int ew=_envIop->info().format().width();
        const int eh=_envIop->info().format().height();
        _envIop->request(0,0,ew,eh,Mask_RGB,1);
    }
}

// _open runs once on the main thread after all _request calls
// and before any engine threads — safe to read env data here
void VDBRenderIop::_open() {
    Iop::_open();
    if(_envDirty&&_envIop){
        cacheEnvMap(_envIop);
        _envDirty=false;
    }
}

void VDBRenderIop::engine(int y,int x,int r,ChannelMask channels,Row&row) {
    // Read BG row if connected
    Row bgRow(x,r);
    bool hasBg=false;
    {Iop*bg=dynamic_cast<Iop*>(Op::input(0));
     if(bg){bg->get(y,x,r,channels,bgRow);hasBg=true;}}

    if(!_gridValid||!_camValid||(!_floatGrid&&!_tempGrid&&!_flameGrid)){
        // Pass through BG or black
        foreach(z,channels){float*p=row.writable(z);
            const float*bp=hasBg?bgRow[z]+x:nullptr;
            for(int i=x;i<r;++i)p[i]=bp?bp[i-x]:0;}
        return;}
    const Format&fmt=format();int W=fmt.width(),H=fmt.height();
    double halfW=_halfW,halfH=_halfW*(double)H/(double)W;
    float*rO=row.writable(Chan_Red),*gO=row.writable(Chan_Green),*bO=row.writable(Chan_Blue),*aO=row.writable(Chan_Alpha);
    ColorScheme scheme=static_cast<ColorScheme>(_colorScheme);
    float gA[3]={(float)_gradStart[0],(float)_gradStart[1],(float)_gradStart[2]};
    float gB[3]={(float)_gradEnd[0],(float)_gradEnd[1],(float)_gradEnd[2]};

    // Get BG channel pointers
    const float*bgR=hasBg?bgRow[Chan_Red]:nullptr;
    const float*bgG=hasBg?bgRow[Chan_Green]:nullptr;
    const float*bgB=hasBg?bgRow[Chan_Blue]:nullptr;
    const float*bgA=hasBg?bgRow[Chan_Alpha]:nullptr;
    for(int ix=x;ix<r;++ix){
        double ndcX=(ix+.5)/(double)W*2-1,ndcY=(y+.5)/(double)H*2-1;
        double rcx=ndcX*halfW,rcy=ndcY*halfH,rcz=-1;
        double rdx=_camRot[0][0]*rcx+_camRot[1][0]*rcy+_camRot[2][0]*rcz;
        double rdy=_camRot[0][1]*rcx+_camRot[1][1]*rcy+_camRot[2][1]*rcz;
        double rdz=_camRot[0][2]*rcx+_camRot[1][2]*rcy+_camRot[2][2]*rcz;
        double len=std::sqrt(rdx*rdx+rdy*rdy+rdz*rdz);if(len>1e-8){rdx/=len;rdy/=len;rdz/=len;}
        double ox=_camOrigin[0],oy=_camOrigin[1],oz=_camOrigin[2];
        if(_hasVolumeXform){
            double tx=_volInv[0][0]*ox+_volInv[1][0]*oy+_volInv[2][0]*oz+_volInv[3][0];
            double ty=_volInv[0][1]*ox+_volInv[1][1]*oy+_volInv[2][1]*oz+_volInv[3][1];
            double tz=_volInv[0][2]*ox+_volInv[1][2]*oy+_volInv[2][2]*oz+_volInv[3][2];ox=tx;oy=ty;oz=tz;
            double dx2=_volInv[0][0]*rdx+_volInv[1][0]*rdy+_volInv[2][0]*rdz;
            double dy2=_volInv[0][1]*rdx+_volInv[1][1]*rdy+_volInv[2][1]*rdz;
            double dz2=_volInv[0][2]*rdx+_volInv[1][2]*rdy+_volInv[2][2]*rdz;
            len=std::sqrt(dx2*dx2+dy2*dy2+dz2*dz2);if(len>1e-8){dx2/=len;dy2/=len;dz2/=len;}rdx=dx2;rdy=dy2;rdz=dz2;}
        openvdb::Vec3d rayO(ox,oy,oz),rayD(rdx,rdy,rdz);
        float ri=(float)_rampIntensity;
        float R=0,G=0,B=0,A=0;
        if(scheme==kLit){marchRay(rayO,rayD,R,G,B,A);R*=ri;G*=ri;B*=ri;}
        else if(scheme==kExplosion){marchRayExplosion(rayO,rayD,R,G,B,A);R*=ri;G*=ri;B*=ri;}
        else{float den=0,alpha=0;marchRayDensity(rayO,rayD,den,alpha);Color3 c=evalRamp(scheme,den,gA,gB,_tempMin,_tempMax);
            R=c.r*ri*alpha;G=c.g*ri*alpha;B=c.b*ri*alpha;A=alpha;}

        // Composite volume OVER background (premultiplied over)
        if(hasBg){
            float inv=1.0f-A;
            rO[ix]=R+bgR[ix]*inv;
            gO[ix]=G+bgG[ix]*inv;
            bO[ix]=B+bgB[ix]*inv;
            aO[ix]=A+bgA[ix]*inv;
        }else{
            rO[ix]=R;gO[ix]=G;bO[ix]=B;aO[ix]=A;
        }
    }
}

// ═══ Environment Map ═══

void VDBRenderIop::cacheEnvMap(Iop* envIop) {
    _hasEnvMap=false;
    if(!envIop)return;
    envIop->open();
    const Format&ef=envIop->info().format();
    const int ew=ef.width(),eh=ef.height();
    if(ew<2||eh<2)return;

    const int cw=kEnvRes,ch=kEnvRes/2;
    for(int cy=0;cy<ch;++cy){
        int sy=std::clamp(cy*eh/ch, 0, eh-1);
        Row row(0,ew);
        envIop->get(sy,0,ew,Mask_RGB,row);
        for(int cx=0;cx<cw;++cx){
            int sx=std::clamp(cx*ew/cw, 0, ew-1);
            float rv=0,gv=0,bv=0;
            const float*rp=row[Chan_Red];   if(rp) rv=rp[sx];
            const float*gp=row[Chan_Green]; if(gp) gv=gp[sx];
            const float*bp=row[Chan_Blue];  if(bp) bv=bp[sx];
            _envMap[cx][cy][0]=rv;
            _envMap[cx][cy][1]=gv;
            _envMap[cx][cy][2]=bv;
        }
    }
    _hasEnvMap=true;
}

void VDBRenderIop::sampleEnv(const openvdb::Vec3d& dir, float& r, float& g, float& b) const {
    // Latlong: u = atan2(x,-z)/(2pi)+0.5, v = asin(y)/pi+0.5
    double u=std::atan2(dir[0],-dir[2])/(2*M_PI)+0.5;
    double v=std::asin(std::clamp(dir[1],-1.0,1.0))/M_PI+0.5;
    // Apply env rotation
    u+=_envRotate/360.0;
    u-=std::floor(u); // wrap to 0-1
    int cx=std::clamp((int)(u*kEnvRes),0,kEnvRes-1);
    int cy=std::clamp((int)(v*(kEnvRes/2)),0,kEnvRes/2-1);
    r=_envMap[cx][cy][0];
    g=_envMap[cx][cy][1];
    b=_envMap[cx][cy][2];
}

// ═══ Multi-scatter direction sets ═══

static const openvdb::Vec3d kDirs6[] = {
    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
static const openvdb::Vec3d kDirs8[] = {
    {0.577,0.577,0.577},{0.577,0.577,-0.577},{0.577,-0.577,0.577},{0.577,-0.577,-0.577},
    {-0.577,0.577,0.577},{-0.577,0.577,-0.577},{-0.577,-0.577,0.577},{-0.577,-0.577,-0.577}};
static const openvdb::Vec3d kDirs12[] = {
    {0.707,0.707,0},{0.707,-0.707,0},{-0.707,0.707,0},{-0.707,-0.707,0},
    {0.707,0,0.707},{0.707,0,-0.707},{-0.707,0,0.707},{-0.707,0,-0.707},
    {0,0.707,0.707},{0,0.707,-0.707},{0,-0.707,0.707},{0,-0.707,-0.707}};

// ═══ marchRay — Lit mode (HDDA + trilinear) ═══
// [v2 TODO] Move inner loop to CUDA kernel with NanoVDB accessor

void VDBRenderIop::marchRay(const openvdb::Vec3d&origin,const openvdb::Vec3d&dir,float&outR,float&outG,float&outB,float&outA) const {
    outR=outG=outB=outA=0;if(!_floatGrid)return;
    const auto&xf=_floatGrid->transform();
    auto acc=_floatGrid->getConstAccessor();
    auto shAcc=_floatGrid->getConstAccessor(); // reused for all shadow rays
    double step=1.0/(std::max(_quality,1.0)*std::max(_quality,1.0)),ext=_extinction,scat=_scattering;
    double g=std::clamp(_anisotropy,-.999,.999),g2=g*g,hgN=(1-g2)/(4*M_PI);
    int nSh=std::max(1,_shadowSteps);double bDiag=(_bboxMax-_bboxMin).length(),shStep=bDiag/(nSh*2);
    double T=1,aR=0,aG=0,aB=0;
    std::unique_ptr<openvdb::FloatGrid::ConstAccessor> tAcc,fAcc;
    if(_hasTempGrid&&_tempGrid)tAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_tempGrid->getConstAccessor());
    if(_hasFlameGrid&&_flameGrid)fAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_flameGrid->getConstAccessor());

    // Lambda: shade one sample at world position wP, index position iP
    auto shadeSample=[&](const openvdb::Vec3d&wP,const openvdb::Vec3d&iP){
        // Trilinear density sample [v2 TODO: NanoVDB texture lookup]
        float density=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
        if(density<=1e-6f)return;
        double se=density*ext,ss=density*scat;
        // Direct lighting with shadow rays
        for(const auto&lt:_lights){openvdb::Vec3d lD;
            if(lt.isPoint){lD=openvdb::Vec3d(lt.pos[0]-wP[0],lt.pos[1]-wP[1],lt.pos[2]-wP[2]);double l=lD.length();if(l>1e-8)lD/=l;else lD=openvdb::Vec3d(0,1,0);}
            else lD=openvdb::Vec3d(lt.dir[0],lt.dir[1],lt.dir[2]);
            double cosT=-(dir[0]*lD[0]+dir[1]*lD[1]+dir[2]*lD[2]);
            double ph;if(std::abs(g)<.001)ph=1./(4*M_PI);else{double dn=1+g2-2*g*cosT;ph=hgN/std::pow(dn,1.5);}
            double phS=ph*4*M_PI;
            double lT=1;
            for(int i=0;i<nSh;++i){auto lw=wP+((i+1)*shStep)*lD;bool in=true;
                for(int a=0;a<3;++a)if(lw[a]<_bboxMin[a]||lw[a]>_bboxMax[a]){in=false;break;}if(!in)break;
                auto li=xf.worldToIndex(lw);
                lT*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,li)*ext*_shadowDensity*shStep);
                if(lT<.001)break;}
            double ctr=ss*phS*lT*T*step;aR+=ctr*lt.color[0];aG+=ctr*lt.color[1];aB+=ctr*lt.color[2];}
        // Ambient
        if(_ambientIntensity>0){double amb=ss*_ambientIntensity*T*step;aR+=amb;aG+=amb;aB+=amb;}
        // Environment map
        if(_hasEnvMap&&_envIntensity>0){
            int nEnv=6+(int)(std::clamp(_envDiffuse,0.0,1.0)*20);
            const openvdb::Vec3d*eDirs[3]={kDirs6,kDirs8,kDirs12};int eDirCnt[3]={6,8,12};
            int used=0;
            for(int dS=0;dS<3&&used<nEnv;++dS)for(int di=0;di<eDirCnt[dS]&&used<nEnv;++di){
                openvdb::Vec3d eDir=eDirs[dS][di];
                openvdb::Vec3d wDir=eDir;
                if(_hasVolumeXform){
                    wDir=openvdb::Vec3d(
                        _volFwd[0][0]*eDir[0]+_volFwd[1][0]*eDir[1]+_volFwd[2][0]*eDir[2],
                        _volFwd[0][1]*eDir[0]+_volFwd[1][1]*eDir[1]+_volFwd[2][1]*eDir[2],
                        _volFwd[0][2]*eDir[0]+_volFwd[1][2]*eDir[1]+_volFwd[2][2]*eDir[2]);
                    double wl=wDir.length();if(wl>1e-8)wDir/=wl;}
                float eR,eG,eB;sampleEnv(wDir,eR,eG,eB);
                double eT=1;
                for(int si=0;si<nSh;++si){auto ew=wP+((si+1)*shStep)*eDir;bool in=true;
                    for(int a5=0;a5<3;++a5)if(ew[a5]<_bboxMin[a5]||ew[a5]>_bboxMax[a5]){in=false;break;}if(!in)break;
                    auto ei=xf.worldToIndex(ew);
                    eT*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,ei)*ext*_shadowDensity*shStep);
                    if(eT<.001)break;}
                double envC=ss*eT*T*step*_envIntensity*(4*M_PI/nEnv);
                aR+=envC*eR;aG+=envC*eG;aB+=envC*eB;
                ++used;}
        }
        // Multi-scatter bounces
        if(_multiBounces>0){
            double albedo=std::min(scat/(ext+1e-8),1.0);
            double bounceStep=bDiag*0.05;int nBounceSteps=4;
            int nDirs=std::min(std::max(_bounceRays,6),26);
            const openvdb::Vec3d*dirs[3]={kDirs6,kDirs8,kDirs12};int dirCounts[3]={6,8,12};
            double bounceR=0,bounceG=0,bounceB=0,bouncePow=albedo;
            for(int bounce=0;bounce<_multiBounces;++bounce){
                double bR=0,bG=0,bB=0;int su=0;
                for(int dSet=0;dSet<3&&su<nDirs;++dSet)for(int di=0;di<dirCounts[dSet]&&su<nDirs;++di){
                    openvdb::Vec3d bDir=dirs[dSet][di];
                    for(int bs=1;bs<=nBounceSteps;++bs){
                        auto bP=wP+(bs*bounceStep)*bDir;bool inside=true;
                        for(int a3=0;a3<3;++a3)if(bP[a3]<_bboxMin[a3]||bP[a3]>_bboxMax[a3]){inside=false;break;}
                        if(!inside)break;
                        auto bI=xf.worldToIndex(bP);
                        float bDen=openvdb::tools::BoxSampler::sample(acc,bI)*(float)_densityMix;
                        if(bDen<1e-6f)continue;
                        for(const auto&lt2:_lights){openvdb::Vec3d lD2;
                            if(lt2.isPoint){lD2=openvdb::Vec3d(lt2.pos[0]-bP[0],lt2.pos[1]-bP[1],lt2.pos[2]-bP[2]);
                                double l2=lD2.length();if(l2>1e-8)lD2/=l2;else continue;}
                            else lD2=openvdb::Vec3d(lt2.dir[0],lt2.dir[1],lt2.dir[2]);
                            double lT2=1;
                            for(int si=0;si<3;++si){auto lw2=bP+((si+1)*shStep*2)*lD2;bool in2=true;
                                for(int a4=0;a4<3;++a4)if(lw2[a4]<_bboxMin[a4]||lw2[a4]>_bboxMax[a4]){in2=false;break;}if(!in2)break;
                                auto li2=xf.worldToIndex(lw2);
                                lT2*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,li2)*ext*_shadowDensity*shStep*2);
                                if(lT2<.001)break;}
                            double c2=bDen*scat*lT2;bR+=c2*lt2.color[0];bG+=c2*lt2.color[1];bB+=c2*lt2.color[2];}
                    }++su;}
                if(su>0){double norm=bouncePow/(su*nBounceSteps);bounceR+=bR*norm;bounceG+=bG*norm;bounceB+=bB*norm;}
                bouncePow*=albedo;}
            double msc=T*step;aR+=bounceR*msc;aG+=bounceG*msc;aB+=bounceB*msc;
        }
        // Temperature emission
        if(tAcc){float tv=openvdb::tools::BoxSampler::sample(*tAcc,iP)*(float)_tempMix;if(tv>.001f){
            double normT=std::clamp((double)tv,_tempMin,_tempMax);Color3 bb=blackbody(normT);
            double tS=std::clamp((tv-_tempMin)/(_tempMax-_tempMin+1e-6),0.,1.);
            double em=_emissionIntensity*tS*T*step;aR+=bb.r*em;aG+=bb.g*em;aB+=bb.b*em;}}
        // Flame emission
        if(fAcc){float fv=openvdb::tools::BoxSampler::sample(*fAcc,iP)*(float)_flameMix;if(fv>.001f){
            Color3 fb=blackbody(std::clamp(_tempMin+fv*(_tempMax-_tempMin),_tempMin,_tempMax));
            double fem=_flameIntensity*fv*T*step;aR+=fb.r*fem;aG+=fb.g*fem;aB+=fb.b*fem;}}
        T*=std::exp(-se*step);
    };

    // HDDA traversal: skip empty space between active leaf nodes
    if(_volRI){
        VRI vri(*_volRI); // thread-safe shallow copy
        openvdb::math::Ray<double> wRay(origin,dir);
        if(!vri.setWorldRay(wRay)){outR=outG=outB=outA=0;return;}
        double it0,it1;
        while(vri.march(it0,it1)&&T>.001){
            auto wS=vri.getWorldPos(it0),wE=vri.getWorldPos(it1);
            double wT0=(wS-origin).dot(dir),wT1=(wE-origin).dot(dir);
            if(wT1<=0)continue;if(wT0<0)wT0=0;
            for(double t2=wT0;t2<wT1&&T>.001;t2+=step){
                auto wP=origin+t2*dir;auto iP=xf.worldToIndex(wP);
                shadeSample(wP,iP);}
        }
    }else{
        // Fallback: manual AABB (shouldn't happen if grid is valid)
        double tEnter=0,tExit=1e9;
        for(int a=0;a<3;++a){double inv=(std::abs(dir[a])>1e-8)?1./dir[a]:1e38;
            double t0=(_bboxMin[a]-origin[a])*inv,t1=(_bboxMax[a]-origin[a])*inv;
            if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}
        if(tEnter>=tExit||tExit<=0)return;
        for(double t2=tEnter;t2<tExit&&T>.001;t2+=step){
            auto wP=origin+t2*dir;auto iP=xf.worldToIndex(wP);
            shadeSample(wP,iP);}
    }
    outR=(float)aR;outG=(float)aG;outB=(float)aB;outA=(float)(1-T);
}

// ═══ marchRayExplosion — smoke + fire (HDDA + trilinear) ═══
// [v2 TODO] Move inner loop to CUDA kernel with NanoVDB accessor

void VDBRenderIop::marchRayExplosion(const openvdb::Vec3d&origin,const openvdb::Vec3d&dir,float&outR,float&outG,float&outB,float&outA) const {
    outR=outG=outB=outA=0;
    openvdb::FloatGrid::Ptr xfGrid=_floatGrid?_floatGrid:(_tempGrid?_tempGrid:_flameGrid);
    if(!xfGrid)return;
    const auto&xf=xfGrid->transform();
    double step=1.0/(std::max(_quality,1.0)*std::max(_quality,1.0)),ext=_extinction,scat=_scattering;
    double g=std::clamp(_anisotropy,-.999,.999),g2=g*g,hgN=(1-g2)/(4*M_PI);
    int nSh=std::max(1,_shadowSteps);double bDiag=(_bboxMax-_bboxMin).length(),shStep=bDiag/(nSh*2);
    double T=1,aR=0,aG=0,aB=0;
    std::unique_ptr<openvdb::FloatGrid::ConstAccessor> dAcc,tAcc,fAcc;
    openvdb::FloatGrid::ConstAccessor shAcc=xfGrid->getConstAccessor(); // shadow ray accessor
    if(_floatGrid)dAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_floatGrid->getConstAccessor());
    if(_hasTempGrid&&_tempGrid)tAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_tempGrid->getConstAccessor());
    if(_hasFlameGrid&&_flameGrid)fAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_flameGrid->getConstAccessor());

    auto shadeSampleExplosion=[&](const openvdb::Vec3d&wP,const openvdb::Vec3d&iP){
        // FIRE: temperature emission (self-luminous, trilinear)
        if(tAcc){float tv=openvdb::tools::BoxSampler::sample(*tAcc,iP)*(float)_tempMix;if(tv>.001f){
            double normT=std::clamp((double)tv,_tempMin,_tempMax);Color3 bb=blackbody(normT);
            double tS=std::clamp((tv-_tempMin)/(_tempMax-_tempMin+1e-6),0.,1.);
            double em=_emissionIntensity*tS*T*step;aR+=bb.r*em;aG+=bb.g*em;aB+=bb.b*em;}}
        // FIRE: flame emission
        if(fAcc){float fv=openvdb::tools::BoxSampler::sample(*fAcc,iP)*(float)_flameMix;if(fv>.001f){
            Color3 fb=blackbody(std::clamp(_tempMin+fv*(_tempMax-_tempMin),_tempMin,_tempMax));
            double fem=_flameIntensity*fv*T*step;aR+=fb.r*fem;aG+=fb.g*fem;aB+=fb.b*fem;}}
        // SMOKE: scatter + absorption
        if(dAcc){float density=openvdb::tools::BoxSampler::sample(*dAcc,iP)*(float)_densityMix;
            if(density>1e-6f){double se=density*ext,ss=density*scat;
                for(const auto&lt:_lights){openvdb::Vec3d lD;
                    if(lt.isPoint){lD=openvdb::Vec3d(lt.pos[0]-wP[0],lt.pos[1]-wP[1],lt.pos[2]-wP[2]);double l=lD.length();if(l>1e-8)lD/=l;else lD=openvdb::Vec3d(0,1,0);}
                    else lD=openvdb::Vec3d(lt.dir[0],lt.dir[1],lt.dir[2]);
                    double cosT=-(dir[0]*lD[0]+dir[1]*lD[1]+dir[2]*lD[2]);
                    double ph;if(std::abs(g)<.001)ph=1./(4*M_PI);else{double dn=1+g2-2*g*cosT;ph=hgN/std::pow(dn,1.5);}
                    double phS=ph*4*M_PI;
                    double lT=1;
                    for(int i=0;i<nSh;++i){auto lw=wP+((i+1)*shStep)*lD;bool in=true;
                        for(int a2=0;a2<3;++a2)if(lw[a2]<_bboxMin[a2]||lw[a2]>_bboxMax[a2]){in=false;break;}if(!in)break;
                        auto li=xf.worldToIndex(lw);
                        lT*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,li)*ext*_shadowDensity*shStep);
                        if(lT<.001)break;}
                    double ctr=ss*phS*lT*T*step;aR+=ctr*lt.color[0];aG+=ctr*lt.color[1];aB+=ctr*lt.color[2];}
                if(_ambientIntensity>0){double amb=ss*_ambientIntensity*T*step;aR+=amb;aG+=amb;aB+=amb;}
                // Environment map
                if(_hasEnvMap&&_envIntensity>0){
                    int nEnv=6+(int)(std::clamp(_envDiffuse,0.0,1.0)*20);
                    const openvdb::Vec3d*eDirs[3]={kDirs6,kDirs8,kDirs12};int eDirCnt[3]={6,8,12};int used=0;
                    for(int dS=0;dS<3&&used<nEnv;++dS)for(int di=0;di<eDirCnt[dS]&&used<nEnv;++di){
                        openvdb::Vec3d eDir=eDirs[dS][di],wDir=eDir;
                        if(_hasVolumeXform){wDir=openvdb::Vec3d(
                            _volFwd[0][0]*eDir[0]+_volFwd[1][0]*eDir[1]+_volFwd[2][0]*eDir[2],
                            _volFwd[0][1]*eDir[0]+_volFwd[1][1]*eDir[1]+_volFwd[2][1]*eDir[2],
                            _volFwd[0][2]*eDir[0]+_volFwd[1][2]*eDir[1]+_volFwd[2][2]*eDir[2]);
                            double wl=wDir.length();if(wl>1e-8)wDir/=wl;}
                        float eR,eG,eB;sampleEnv(wDir,eR,eG,eB);
                        double eT=1;
                        for(int si=0;si<nSh;++si){auto ew=wP+((si+1)*shStep)*eDir;bool in2=true;
                            for(int a5=0;a5<3;++a5)if(ew[a5]<_bboxMin[a5]||ew[a5]>_bboxMax[a5]){in2=false;break;}if(!in2)break;
                            auto ei=xf.worldToIndex(ew);
                            eT*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,ei)*ext*_shadowDensity*shStep);
                            if(eT<.001)break;}
                        double envC=ss*eT*T*step*_envIntensity*(4*M_PI/nEnv);
                        aR+=envC*eR;aG+=envC*eG;aB+=envC*eB;++used;}
                }
                // Multi-scatter
                if(_multiBounces>0){
                    double albedo=std::min(scat/(ext+1e-8),1.0);double bounceStep2=bDiag*0.05;
                    int nBSteps=4,nDirs=std::min(std::max(_bounceRays,6),26);
                    const openvdb::Vec3d*dirs[3]={kDirs6,kDirs8,kDirs12};int dirCounts[3]={6,8,12};
                    double bounceR=0,bounceG=0,bounceB=0,bouncePow=albedo;
                    for(int bounce=0;bounce<_multiBounces;++bounce){double bR=0,bG=0,bB=0;int su=0;
                        for(int dS=0;dS<3&&su<nDirs;++dS)for(int di=0;di<dirCounts[dS]&&su<nDirs;++di){
                            openvdb::Vec3d bDir=dirs[dS][di];
                            for(int bs=1;bs<=nBSteps;++bs){auto bP=wP+(bs*bounceStep2)*bDir;bool inside=true;
                                for(int a3=0;a3<3;++a3)if(bP[a3]<_bboxMin[a3]||bP[a3]>_bboxMax[a3]){inside=false;break;}
                                if(!inside)break;auto bI=xf.worldToIndex(bP);
                                float bDen=openvdb::tools::BoxSampler::sample(*dAcc,bI)*(float)_densityMix;
                                if(bDen<1e-6f)continue;
                                for(const auto&lt2:_lights){openvdb::Vec3d lD2;
                                    if(lt2.isPoint){lD2=openvdb::Vec3d(lt2.pos[0]-bP[0],lt2.pos[1]-bP[1],lt2.pos[2]-bP[2]);
                                        double l2=lD2.length();if(l2>1e-8)lD2/=l2;else continue;}
                                    else lD2=openvdb::Vec3d(lt2.dir[0],lt2.dir[1],lt2.dir[2]);
                                    double lT2=1;
                                    for(int si=0;si<3;++si){auto lw2=bP+((si+1)*shStep*2)*lD2;bool in2=true;
                                        for(int a4=0;a4<3;++a4)if(lw2[a4]<_bboxMin[a4]||lw2[a4]>_bboxMax[a4]){in2=false;break;}if(!in2)break;
                                        auto li2=xf.worldToIndex(lw2);
                                        lT2*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,li2)*ext*_shadowDensity*shStep*2);
                                        if(lT2<.001)break;}
                                    double c2=bDen*scat*lT2;bR+=c2*lt2.color[0];bG+=c2*lt2.color[1];bB+=c2*lt2.color[2];}
                            }++su;}
                        if(su>0){double norm=bouncePow/(su*nBSteps);bounceR+=bR*norm;bounceG+=bG*norm;bounceB+=bB*norm;}
                        bouncePow*=albedo;}
                    double msc=T*step;aR+=bounceR*msc;aG+=bounceG*msc;aB+=bounceB*msc;
                }
                T*=std::exp(-se*step);}
        }
    };

    // HDDA traversal or AABB fallback
    if(_volRI){
        VRI vri(*_volRI);openvdb::math::Ray<double> wRay(origin,dir);
        if(!vri.setWorldRay(wRay))return;
        double it0,it1;
        while(vri.march(it0,it1)&&T>.001){
            auto wS=vri.getWorldPos(it0),wE=vri.getWorldPos(it1);
            double wT0=(wS-origin).dot(dir),wT1=(wE-origin).dot(dir);
            if(wT1<=0)continue;if(wT0<0)wT0=0;
            for(double t2=wT0;t2<wT1&&T>.001;t2+=step){
                auto wP=origin+t2*dir;auto iP=xf.worldToIndex(wP);
                shadeSampleExplosion(wP,iP);}
        }
    }else{
        double tEnter=0,tExit=1e9;
        for(int a=0;a<3;++a){double inv=(std::abs(dir[a])>1e-8)?1./dir[a]:1e38;
            double t0=(_bboxMin[a]-origin[a])*inv,t1=(_bboxMax[a]-origin[a])*inv;
            if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}
        if(tEnter>=tExit||tExit<=0)return;
        for(double t2=tEnter;t2<tExit&&T>.001;t2+=step){
            auto wP=origin+t2*dir;auto iP=xf.worldToIndex(wP);
            shadeSampleExplosion(wP,iP);}
    }
    outR=(float)aR;outG=(float)aG;outB=(float)aB;outA=(float)(1-T);
}

// ═══ marchRayDensity — ramp modes (HDDA + trilinear) ═══

void VDBRenderIop::marchRayDensity(const openvdb::Vec3d&origin,const openvdb::Vec3d&dir,float&outD,float&outA) const {
    outD=0;outA=0;if(!_floatGrid)return;
    auto acc=_floatGrid->getConstAccessor();const auto&xf=_floatGrid->transform();
    bool useTG=_hasTempGrid&&_tempGrid&&_colorScheme==kBlackbody;
    std::unique_ptr<openvdb::FloatGrid::ConstAccessor> tA;
    if(useTG)tA=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_tempGrid->getConstAccessor());
    double step=1.0/(std::max(_quality,1.0)*std::max(_quality,1.0)),ext=_extinction,T=1,wV=0;

    auto shadeDensity=[&](const openvdb::Vec3d&iP){
        float d=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
        if(d>1e-6f){double se=d*ext,ab=T*(1-std::exp(-se*step));
            float val=d;if(useTG){float tv=openvdb::tools::BoxSampler::sample(*tA,iP)*(float)_tempMix;
                val=(float)std::clamp((tv-_tempMin)/(_tempMax-_tempMin),0.,1.);}
            wV+=val*ab;T*=std::exp(-se*step);}
    };

    if(_volRI){
        VRI vri(*_volRI);openvdb::math::Ray<double> wRay(origin,dir);
        if(!vri.setWorldRay(wRay))return;
        double it0,it1;
        while(vri.march(it0,it1)&&T>.001){
            auto wS=vri.getWorldPos(it0),wE=vri.getWorldPos(it1);
            double wT0=(wS-origin).dot(dir),wT1=(wE-origin).dot(dir);
            if(wT1<=0)continue;if(wT0<0)wT0=0;
            for(double t2=wT0;t2<wT1&&T>.001;t2+=step){
                auto iP=xf.worldToIndex(origin+t2*dir);shadeDensity(iP);}
        }
    }else{
        double tEnter=0,tExit=1e9;
        for(int a=0;a<3;++a){double inv=(std::abs(dir[a])>1e-8)?1./dir[a]:1e38;
            double t0=(_bboxMin[a]-origin[a])*inv,t1=(_bboxMax[a]-origin[a])*inv;
            if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}
        if(tEnter>=tExit||tExit<=0)return;
        for(double t2=tEnter;t2<tExit&&T>.001;t2+=step){
            auto iP=xf.worldToIndex(origin+t2*dir);shadeDensity(iP);}
    }
    double alpha=1-T;outD=(alpha>1e-6)?std::clamp((float)(wV/alpha),0.f,1.f):0;outA=(float)alpha;
}

// ═══ Deep Interface ═══

void VDBRenderIop::getDeepRequests(Box box, const ChannelSet& channels,
                                    int count, std::vector<RequestData>& reqData) {
}

bool VDBRenderIop::doDeepEngine(Box box, const ChannelSet& channels,
                                 DeepOutputPlane& plane) {
    if(!_gridValid||!_camValid||(!_floatGrid&&!_tempGrid&&!_flameGrid)){
        ChannelSet outChans=Mask_RGBA;outChans+=Chan_DeepFront;outChans+=Chan_DeepBack;
        plane=DeepOutputPlane(outChans,box);
        for(int y=box.y();y<box.t();++y)for(int x=box.x();x<box.r();++x)plane.addHole();
        return true;
    }

    const Format&fmt=info_.format();
    int W=fmt.width(),H=fmt.height();
    double halfW=_halfW,halfH=_halfW*(double)H/(double)W;

    ChannelSet outChans=Mask_RGBA;
    outChans+=Chan_DeepFront;outChans+=Chan_DeepBack;
    plane=DeepOutputPlane(outChans,box);

    double step=1.0/(std::max(_quality,1.0)*std::max(_quality,1.0));
    double ext=_extinction,scat=_scattering;
    double g=std::clamp(_anisotropy,-.999,.999),g2=g*g,hgN=(1-g2)/(4*M_PI);
    int nSh=std::max(1,_shadowSteps);
    double bDiag=(_bboxMax-_bboxMin).length(),shStep=bDiag/(nSh*2);
    float ri=(float)_rampIntensity;
    int maxSamples=std::max(1,_deepSamples);
    double deepStep=step*std::max(1,(int)std::ceil(bDiag/(step*maxSamples)));

    

    for(int iy=box.y();iy<box.t();++iy){
        for(int ix=box.x();ix<box.r();++ix){
            double ndcX=(ix+.5)/(double)W*2-1,ndcY=(iy+.5)/(double)H*2-1;
            double rcx=ndcX*halfW,rcy=ndcY*halfH,rcz=-1;
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
                if(len>1e-8){dx2/=len;dy2/=len;dz2/=len;}rdx=dx2;rdy=dy2;rdz=dz2;}

            openvdb::Vec3d rayO(ox,oy,oz),rayD(rdx,rdy,rdz);

            double tEnter=0,tExit=1e9;
            for(int a=0;a<3;++a){
                double inv=(std::abs(rayD[a])>1e-8)?1./rayD[a]:1e38;
                double t0=(_bboxMin[a]-rayO[a])*inv,t1=(_bboxMax[a]-rayO[a])*inv;
                if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}

            if(tEnter>=tExit||tExit<=0){plane.addHole();continue;}

            openvdb::FloatGrid::Ptr xfGrid=_floatGrid?_floatGrid:(_tempGrid?_tempGrid:_flameGrid);
            const auto&xf=xfGrid->transform();
            std::unique_ptr<openvdb::FloatGrid::ConstAccessor> dAcc,tAcc,fAcc;
            if(_floatGrid)dAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_floatGrid->getConstAccessor());
            if(_hasTempGrid&&_tempGrid)tAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_tempGrid->getConstAccessor());
            if(_hasFlameGrid&&_flameGrid)fAcc=std::make_unique<openvdb::FloatGrid::ConstAccessor>(_flameGrid->getConstAccessor());

            DeepOutPixel pixel;
            double T=1.0;
            int sampleCount=0;

            for(double t=tEnter;t<tExit&&T>0.001&&sampleCount<maxSamples;t+=deepStep){
                double tEnd=std::min(t+deepStep,tExit);
                float slabR=0,slabG=0,slabB=0;
                double slabT=T;

                for(double ts=t;ts<tEnd&&T>0.001;ts+=step){
                    auto wP=rayO+ts*rayD;
                    auto iP=xf.worldToIndex(wP);
                    openvdb::Coord ijk((int)std::floor(iP[0]),(int)std::floor(iP[1]),(int)std::floor(iP[2]));

                    if(tAcc){float tv=tAcc->getValue(ijk)*(float)_tempMix;if(tv>.001f){
                        double normT=std::clamp((double)tv,_tempMin,_tempMax);Color3 bb=blackbody(normT);
                        double tS=std::clamp((tv-_tempMin)/(_tempMax-_tempMin+1e-6),0.,1.);
                        double em=_emissionIntensity*tS*T*step;
                        slabR+=bb.r*(float)em;slabG+=bb.g*(float)em;slabB+=bb.b*(float)em;}}

                    if(fAcc){float fv=fAcc->getValue(ijk)*(float)_flameMix;if(fv>.001f){
                        Color3 fb=blackbody(std::clamp(_tempMin+fv*(_tempMax-_tempMin),_tempMin,_tempMax));
                        double fem=_flameIntensity*fv*T*step;
                        slabR+=fb.r*(float)fem;slabG+=fb.g*(float)fem;slabB+=fb.b*(float)fem;}}

                    if(dAcc){float density=dAcc->getValue(ijk)*(float)_densityMix;
                        if(density>1e-6f){double se=density*ext,ss=density*scat;
                            for(const auto&lt:_lights){openvdb::Vec3d lD;
                                if(lt.isPoint){lD=openvdb::Vec3d(lt.pos[0]-wP[0],lt.pos[1]-wP[1],lt.pos[2]-wP[2]);
                                    double l2=lD.length();if(l2>1e-8)lD/=l2;else lD=openvdb::Vec3d(0,1,0);}
                                else lD=openvdb::Vec3d(lt.dir[0],lt.dir[1],lt.dir[2]);
                                double cosT2=-(rayD[0]*lD[0]+rayD[1]*lD[1]+rayD[2]*lD[2]);
                                double ph;if(std::abs(g)<.001)ph=1./(4*M_PI);
                                else{double dn=1+g2-2*g*cosT2;ph=hgN/std::pow(dn,1.5);}
                                double phS=ph*4*M_PI;
                                double lT=1;
                                if(_floatGrid){auto la=_floatGrid->getConstAccessor();
                                    for(int i=0;i<nSh;++i){auto lw=wP+((i+1)*shStep)*lD;bool in=true;
                                        for(int a2=0;a2<3;++a2)if(lw[a2]<_bboxMin[a2]||lw[a2]>_bboxMax[a2]){in=false;break;}if(!in)break;
                                        auto li=xf.worldToIndex(lw);
                                        lT*=std::exp(-(double)la.getValue(openvdb::Coord((int)std::floor(li[0]),(int)std::floor(li[1]),(int)std::floor(li[2])))*ext*_shadowDensity*shStep);
                                        if(lT<.001)break;}}
                                double ctr=ss*phS*lT*T*step;
                                slabR+=(float)(ctr*lt.color[0]);slabG+=(float)(ctr*lt.color[1]);slabB+=(float)(ctr*lt.color[2]);}
                            if(_ambientIntensity>0){double amb=ss*_ambientIntensity*T*step;
                                slabR+=(float)amb;slabG+=(float)amb;slabB+=(float)amb;}
                            T*=std::exp(-se*step);}}
                }

                float slabAlpha=(float)(slabT-T);
                // For pure emission (no density), alpha would be zero.
                // Synthesize alpha from emission luminance so DeepToImage works.
                float emitLum=slabR*0.2126f+slabG*0.7152f+slabB*0.0722f;
                if(slabAlpha<1e-6f && emitLum>1e-6f){
                    // Clamp to reasonable alpha — emission is additive/premultiplied
                    slabAlpha=std::min(emitLum*ri, 1.0f);
                }
                if(slabAlpha>1e-6f||(slabR+slabG+slabB)>1e-6f){
                    foreach(z,outChans){
                        if(z==Chan_DeepFront)     pixel.push_back((float)t);
                        else if(z==Chan_DeepBack) pixel.push_back((float)tEnd);
                        else if(z==Chan_Red)      pixel.push_back(slabR*ri);
                        else if(z==Chan_Green)    pixel.push_back(slabG*ri);
                        else if(z==Chan_Blue)     pixel.push_back(slabB*ri);
                        else if(z==Chan_Alpha)    pixel.push_back(slabAlpha);
                        else                      pixel.push_back(0.0f);
                    }
                    ++sampleCount;
                }
            }

            if(sampleCount>0)plane.addPixel(pixel);
            else plane.addHole();
        }
    }
    return true;
}

// ═══ Registration ═══
static Op*build(Node*n){return new VDBRenderIop(n);}
const Op::Description VDBRenderIop::desc(VDBRenderIop::CLASS,build);
