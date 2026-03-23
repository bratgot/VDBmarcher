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
    // ═══════════════════════════════════════════════════
    //  TAB: VDBRender
    // ═══════════════════════════════════════════════════
    Tab_knob(f,"VDBRender");

    Text_knob(f,
        "<font size='+2'><b>VDBRender</b></font>"
        " <font color='#888' size='-1'>v2.0</font><br>"
        "<font color='#aaa'>OpenVDB volume ray marcher</font>");

    File_knob(f,&_vdbFilePath,"file","VDB File");
    Tooltip(f,"Path to your .vdb file from Houdini, Ember, or other VDB exporters.\n"
              "Sequences: use #### or %04d in the filename for frame padding.\n"
              "Tip: Enable 'Auto Sequence' to convert numbered files automatically.");
    Bool_knob(f,&_autoSequence,"auto_sequence","Auto Sequence");
    Tooltip(f,"Converts frame numbers in the filename to #### padding.\n"
              "e.g. explosion_0001.vdb becomes explosion_####.vdb\n"
              "and explosion_1.vdb becomes explosion_####.vdb\n"
              "Enable this if your VDB sequence isn't animating.");
    Format_knob(f,&_formats,"format","Format");
    Tooltip(f,"Output resolution when no background plate is connected.\n"
              "If a BG is connected to input 0, its format is used instead.");

    Divider(f,"Grids");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Set the grid names from your VDB file, or click Discover Grids<br>"
        "to auto-detect them. Mix sliders scale values before rendering."
        "</font>");
    Button(f,"discover_grids","Discover Grids");
    Tooltip(f,"Scans the VDB file and auto-fills all grid fields.\n"
              "Finds density, temperature, flame, velocity, and colour grids.\n"
              "Also sets Render Mode to Explosion if fire grids are found.");
    String_knob(f,&_gridName,"grid_name","Density");
    Tooltip(f,"Float grid for smoke opacity and light scattering.\n"
              "Common names from Houdini: density, smoke, soot\n"
              "Leave empty for emission-only rendering (fire without smoke).");
    Double_knob(f,&_densityMix,"density_mix","Density Mix");SetRange(f,0,5);
    Tooltip(f,"Multiplier on density values before shading.\n"
              "0 = invisible. 1 = as exported. 2 = twice as dense.\n"
              "Useful for tweaking without re-simulating.");
    String_knob(f,&_tempGridName,"temp_grid","Temperature");
    Tooltip(f,"Float grid for blackbody emission colour.\n"
              "Common names: temperature, heat, temp\n"
              "Values in Kelvin drive fire colour (orange→white).");
    Double_knob(f,&_tempMix,"temp_mix","Temp Mix");SetRange(f,0,5);
    Tooltip(f,"Multiplier on temperature values.\n"
              "0 = no glow. 1 = original. Higher = brighter fire.");
    String_knob(f,&_flameGridName,"flame_grid","Flames");
    Tooltip(f,"Float grid for additional flame emission.\n"
              "Common names: flame, flames, fire, fuel, burn\n"
              "Adds glow on top of temperature emission.");
    Double_knob(f,&_flameMix,"flame_mix","Flame Mix");SetRange(f,0,5);
    Tooltip(f,"Multiplier on flame values.\n"
              "0 = no flames. 1 = original. Higher = brighter.");
    String_knob(f,&_velGridName,"vel_grid","Velocity");
    Tooltip(f,"Vec3 grid for motion blur.\n"
              "Common names: vel, v, velocity\n"
              "Enable motion blur in the Output tab to use this.");
    String_knob(f,&_colorGridName,"color_grid","Colour");
    Tooltip(f,"Vec3 grid for direct RGB colour from your simulation.\n"
              "Common names: Cd, color, colour, rgb, albedo\n"
              "Overrides the render mode colour when loaded.");
    Int_knob(f,&_frameOffset,"frame_offset","Frame Offset");
    Tooltip(f,"Offsets the timeline frame for sequences.\n"
              "Use negative values to shift the sequence earlier.\n"
              "e.g. -10 makes frame 20 read file frame 10.");

    Divider(f,"Render");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Scene Presets configure all shading values for common volume types.<br>"
        "Switch to Custom to fine-tune individual settings."
        "</font>");
    static const char*presets[]={"Custom","Thin Smoke","Dense Smoke","Fog / Mist",
        "Cumulus Cloud","Fire","Explosion","Pyroclastic","Dust Storm","Steam",nullptr};
    Enumeration_knob(f,&_scenePreset,presets,"scene_preset","Scene Preset");
    Tooltip(f,"One-click setup for common volume types.\n"
              "Sets render mode, extinction, scattering, anisotropy,\n"
              "shadows, emission, and quality all at once.\n"
              "Choose Custom to adjust settings manually.");
    static const char*modes[]={"Lit","Greyscale","Heat","Cool","Blackbody","Custom Gradient","Explosion",nullptr};
    Enumeration_knob(f,&_colorScheme,modes,"color_scheme","Render Mode");
    Tooltip(f,"Lit — full lighting with shadows and phase function\n"
              "Greyscale — quick density preview, no lighting\n"
              "Heat/Cool — artistic temperature ramps\n"
              "Blackbody — physically accurate fire colour\n"
              "Custom Gradient — your own two-colour ramp\n"
              "Explosion — smoke + self-luminous fire combined");
    Double_knob(f,&_rampIntensity,"ramp_intensity","Intensity");SetRange(f,0,10);
    Tooltip(f,"Master brightness for all render modes.\n"
              "Multiplies the final RGB output.\n"
              "Does not affect alpha/opacity.");

    Divider(f,"Shading");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Extinction controls opacity. Scattering controls brightness under<br>"
        "lighting. Anisotropy shifts the scatter direction for backlit or rim effects."
        "</font>");
    Double_knob(f,&_extinction,"extinction","Extinction");SetRange(f,0,100);
    Tooltip(f,"How quickly light is absorbed per unit density.\n"
              "Higher = more opaque volume.\n"
              "Thin smoke: 1-5. Cloud: 10-30. Solid: 50+");
    Double_knob(f,&_scattering,"scattering","Scattering");SetRange(f,0,100);
    Tooltip(f,"How bright the volume appears under direct lighting.\n"
              "Only used in Lit and Explosion modes.\n"
              "0 = pure absorption (dark). Higher = brighter.");
    static const char*aniP[]={"Custom","Isotropic (0.0)","Smoke (0.4)","Dust (0.6)","Cloud (0.76)","Fog (0.8)","Rim Light (-0.4)","Strong Back (-0.7)",nullptr};
    Enumeration_knob(f,&_anisotropyPreset,aniP,"aniso_preset","Anisotropy Preset");
    Tooltip(f,"Quick presets for the Henyey-Greenstein g value.\n"
              "Sets the Anisotropy slider below automatically.");
    Double_knob(f,&_anisotropy,"anisotropy","Anisotropy (g)");SetRange(f,-1,1);
    Tooltip(f,"Controls the direction light scatters through the volume.\n"
              "-1 = backward scatter (rim light / halo effect)\n"
              " 0 = even scatter in all directions\n"
              "+1 = forward scatter (backlit glow, silver lining)\n"
              "Smoke: 0.4, Cloud: 0.76, Dust: 0.6");

    BeginClosedGroup(f,"grp_emission","Emission");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Temperature and flame emission controls for fire and explosion<br>"
        "rendering. Only active when temperature or flame grids are loaded."
        "</font>");
    Double_knob(f,&_tempMin,"temp_min","Temp Min (K)");SetRange(f,0,15000);
    Tooltip(f,"Kelvin temperature at zero emission.\n"
              "Sets the cold/dark end of the temperature range.\n"
              "Candle: 1800K. Fire base: 500K.");
    Double_knob(f,&_tempMax,"temp_max","Temp Max (K)");SetRange(f,0,15000);
    Tooltip(f,"Kelvin temperature at peak emission.\n"
              "Sets the hot/bright end of the temperature range.\n"
              "Fire: 3000K. Explosion: 8000K. Plasma: 15000K.");
    Double_knob(f,&_emissionIntensity,"emission_intensity","Temp Emission");SetRange(f,0,10);
    Tooltip(f,"Brightness of temperature grid emission.\n"
              "Only active when a temperature grid is loaded.\n"
              "Start with 1-5 and adjust to taste.");
    Double_knob(f,&_flameIntensity,"flame_intensity","Flame Emission");SetRange(f,0,20);
    Tooltip(f,"Brightness of flame grid emission.\n"
              "Only active when a flames grid is loaded.\n"
              "Start with 2-10 for visible fire.");
    Divider(f,"Custom Gradient");
    Color_knob(f,_gradStart,"grad_start","Start Color");
    Tooltip(f,"Low density colour for the Custom Gradient render mode.\n"
              "This is the colour at zero density (transparent end).");
    Color_knob(f,_gradEnd,"grad_end","End Color");
    Tooltip(f,"High density colour for the Custom Gradient render mode.\n"
              "This is the colour at peak density (opaque end).");
    EndGroup(f);

    Divider(f,"Lighting");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Connect lights via the scn input (Scene node). If no lights<br>"
        "are found, the fallback light below is used instead."
        "</font>");
    Bool_knob(f,&_useFallbackLight,"use_fallback_light","Fallback Light");
    Tooltip(f,"Enable a default directional light when no scene lights\n"
              "are connected. Disable this to light with environment\n"
              "and ambient only.");
    XYZ_knob(f,_lightDir,"light_dir","Direction");
    Tooltip(f,"Direction toward the fallback light source.\n"
              "Only used when no lights are found in the scene input.\n"
              "Tip: use the 3D viewer to visualize the light direction.");
    Color_knob(f,_lightColor,"light_color","Light Color");
    Tooltip(f,"Fallback light colour.\n"
              "Only used when no scene lights are connected.");
    Double_knob(f,&_lightIntensity,"light_intensity","Light Intensity");SetRange(f,0,50);
    Tooltip(f,"Fallback light brightness.\n"
              "Only used when no scene lights are connected.");
    Double_knob(f,&_ambientIntensity,"ambient","Ambient");SetRange(f,0,5);
    Tooltip(f,"Omnidirectional fill light with no shadows.\n"
              "Lifts the darkest self-shadowed areas of dense volumes.\n"
              "Useful for smoke that looks too dark on the shadow side.");

    BeginClosedGroup(f,"grp_env","Environment Map");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Connect a latlong HDRI to the env input for realistic sky lighting.<br>"
        "The map is sampled from multiple directions with shadow rays."
        "</font>");
    Double_knob(f,&_envIntensity,"env_intensity","Intensity");SetRange(f,0,10);
    Tooltip(f,"Brightness of environment lighting.\n"
              "0 = disabled. 1 = match the HDRI values.\n"
              "Values above 1 boost the environment contribution.");
    Double_knob(f,&_envRotate,"env_rotate","Rotate");SetRange(f,0,360);
    Tooltip(f,"Rotates the environment map horizontally in degrees.\n"
              "0 and 360 are the same. Animatable for spinning setups.\n"
              "Useful for adjusting where the key light falls on the volume.");
    Double_knob(f,&_envDiffuse,"env_diffuse","Diffuse");SetRange(f,0,1);
    Tooltip(f,"How soft or spread out the environment lighting is.\n"
              "0 = sharp directional highlights from the HDRI hotspots\n"
              "0.5 = soft natural spread\n"
              "1 = fully diffuse, even light from all directions\n"
              "Higher values use more samples and render slower.");
    EndGroup(f);

    BeginClosedGroup(f,"grp_info","Technical Reference");
    Text_knob(f,
        "<font size='-1' color='#bbb'>"
        "<b>Radiative Transfer</b> — Beer-Lambert transmittance<br>"
        "with HDDA empty-space skipping and trilinear<br>"
        "BoxSampler interpolation."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Phase</b> — Henyey-Greenstein. <b>Scatter</b> — N-bounce<br>"
        "with 6-26 directions, albedo-weighted energy conservation."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Blackbody</b> — Planckian locus, 1000K red to<br>"
        "10000K blue-white. <b>Motion Blur</b> — velocity grid<br>"
        "ray origin offset across shutter interval."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Grids:</b> density (float) · temperature (float)<br>"
        "· flame (float) · vel (vec3) · Cd (vec3)"
        "</font><br><br>"
        "<font size='-1' color='#888'>"
        "Museth (2013) · Henyey/Greenstein (1941)<br>"
        "· Fong+ (2017) · Novak+ (2018)"
        "</font>");
    EndGroup(f);

    Divider(f,"");
    Text_knob(f,
        "<font size='-1' color='#666'>"
        "<b>Created by Marten Blumen</b> · "
        "github.com/bratgot/VDBmarcher<br>"
        "OpenVDB 12 · C++20 · Nuke 17"
        "</font>");

    // ═══════════════════════════════════════════════════
    //  TAB: Output
    // ═══════════════════════════════════════════════════
    Tab_knob(f,"Output");

    Divider(f,"AOV Passes");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Enable extra output layers for compositing. Each pass<br>"
        "appears as a separate layer in the channel viewer.<br>"
        "Access them with Shuffle or Copy nodes downstream."
        "</font>");
    Bool_knob(f,&_aovDensity,"aov_density","Density");
    Tooltip(f,"Outputs integrated volume density as the vdb_density layer.\n"
              "Greyscale value written to R, G, B channels.\n"
              "Equivalent to the beauty alpha — useful for holdout mattes.");
    Bool_knob(f,&_aovEmission,"aov_emission","Emission");
    Tooltip(f,"Outputs emission contribution as the vdb_emission layer.\n"
              "Full colour RGB — the pre-composite beauty before BG layering.\n"
              "Useful for adjusting fire brightness in comp.");
    Bool_knob(f,&_aovShadow,"aov_shadow","Shadow");
    Tooltip(f,"Outputs shadow transmittance as the vdb_shadow layer.\n"
              "1.0 = fully lit, 0.0 = fully occluded.\n"
              "Greyscale across R, G, B. Useful for shadow manipulation.");
    Bool_knob(f,&_aovDepth,"aov_depth","Depth");
    Tooltip(f,"Outputs first-hit depth as the vdb_depth layer.\n"
              "Camera distance to the first significant density sample.\n"
              "Single channel (red). Useful for depth-of-field or fog cards.");

    Divider(f,"Deep Output");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Deep output generates depth-sorted RGBA slabs for compositing<br>"
        "with CG renders. Connect to DeepMerge for multi-volume or<br>"
        "volume-over-geometry setups."
        "</font>");
    Int_knob(f,&_deepSamples,"deep_samples","Deep Samples");
    Tooltip(f,"Maximum number of depth slabs per pixel.\n"
              "16 = fast preview. 64 = smooth gradients.\n"
              "128 = final quality for deep compositing.");

    Divider(f,"Motion Blur");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Velocity-based motion blur. Set the Velocity grid name<br>"
        "in the VDBRender tab, then enable and configure here."
        "</font>");
    Bool_knob(f,&_motionBlur,"motion_blur","Enable");
    Tooltip(f,"Enable velocity-based motion blur.\n"
              "Requires a velocity grid (vel/v) to be set in the VDBRender tab.\n"
              "Offsets the ray origin across the shutter interval.");
    static const char*shutP[]={"Start (0 to 1)","Centre (-0.5 to 0.5)","End (-1 to 0)","Custom",nullptr};
    Enumeration_knob(f,&_shutterPreset,shutP,"shutter_preset","Shutter");
    Tooltip(f,"Quick shutter presets:\n"
              "Start — blur trails behind the motion direction\n"
              "Centre — blur centred on the current frame\n"
              "End — blur leads ahead of the motion direction\n"
              "Custom — set Open and Close manually below.");
    Double_knob(f,&_shutterOpen,"shutter_open","Open");SetRange(f,-1,0);
    Tooltip(f,"Shutter opening time relative to the current frame.\n"
              "-0.5 = half frame before. -1.0 = one full frame before.\n"
              "Set by the Shutter preset, or use Custom to edit.");
    Double_knob(f,&_shutterClose,"shutter_close","Close");SetRange(f,0,1);
    Tooltip(f,"Shutter closing time relative to the current frame.\n"
              "0.5 = half frame after. 1.0 = one full frame after.\n"
              "Set by the Shutter preset, or use Custom to edit.");
    Int_knob(f,&_motionSamples,"motion_samples","Samples");
    Tooltip(f,"Number of time samples across the shutter interval.\n"
              "2 = fast linear blur. 3 = good quality.\n"
              "5 = very smooth. More = slower render.");

    // ═══════════════════════════════════════════════════
    //  TAB: Quality
    // ═══════════════════════════════════════════════════
    Tab_knob(f,"Quality");

    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Start with a Quality Preset, then fine-tune individual settings.<br>"
        "Draft for layout, Production for review, Final for delivery."
        "</font>");
    static const char*qP[]={"Custom","Draft","Preview","Production","Final","Ultra",nullptr};
    Enumeration_knob(f,&_qualityPreset,qP,"quality_preset","Preset");
    Tooltip(f,"Sets all quality controls at once.\n"
              "Draft = fastest, rough look for layout\n"
              "Preview = quick turnaround for review\n"
              "Production = good quality for client review\n"
              "Final = full quality for delivery\n"
              "Ultra = maximum fidelity, slowest render");
    Double_knob(f,&_quality,"quality","Quality");SetRange(f,1,10);
    Tooltip(f,"Ray march step resolution on a logarithmic scale.\n"
              "Step size = 1/(quality squared).\n"
              "1 = fast preview. 5 = good quality.\n"
              "7 = high quality. 10 = final render.");
    Int_knob(f,&_shadowSteps,"shadow_steps","Shadow Steps");
    Tooltip(f,"Number of shadow ray samples per light source.\n"
              "Controls self-shadow smoothness inside the volume.\n"
              "4-8 = preview. 16-32 = final render.");
    Double_knob(f,&_shadowDensity,"shadow_density","Shadow Density");SetRange(f,0,5);
    Tooltip(f,"Multiplier on extinction for shadow rays.\n"
              "1 = physically correct shadows.\n"
              "Below 1 = lighter, more transparent shadows.\n"
              "Above 1 = darker, heavier shadows.\n"
              "0 = no self-shadowing at all (fully lit volume).");

    Divider(f,"Multiple Scattering");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Simulates light bouncing through the volume. Essential for<br>"
        "realistic clouds and thick fog. More bounces = slower render."
        "</font>");
    static const char*scP[]={"Off","Preview","Thin Volume","Cloud / Fog","Dense Volume","Ultra",nullptr};
    Enumeration_knob(f,&_scatterPreset,scP,"scatter_preset","Scatter Preset");
    Tooltip(f,"Quick scatter quality presets:\n"
              "Off — single scatter only, fastest\n"
              "Preview — 1 bounce, 6 rays\n"
              "Thin Volume — 1 bounce, 14 rays\n"
              "Cloud / Fog — 2 bounces, 14 rays\n"
              "Dense Volume — 3 bounces, 26 rays\n"
              "Ultra — 4 bounces, 26 rays, slowest");
    Int_knob(f,&_multiBounces,"multi_bounces","Bounces");
    Tooltip(f,"Number of extra light bounces through the volume.\n"
              "0 = single scatter only (fastest)\n"
              "1 = one bounce, good for clouds\n"
              "2-3 = dense fog and thick volumes\n"
              "4 = maximum physical realism, slowest");
    Int_knob(f,&_bounceRays,"bounce_rays","Bounce Rays");
    Tooltip(f,"Directions sampled per bounce.\n"
              "6 = axis-aligned (fast but blocky)\n"
              "14 = adds diagonals (smoother)\n"
              "26 = full directional coverage (best quality)");

    Divider(f,"Optimization");
    Bool_knob(f,&_adaptiveStep,"adaptive_step","Adaptive Step Size");
    Tooltip(f,"Automatically takes larger steps in thin regions\n"
              "and smaller steps near dense surfaces.\n"
              "4x in very thin areas, 2x in medium, normal in dense.\n"
              "Typical 2-4x speedup with minimal quality loss.\n"
              "Safe to leave on for most renders.");
    static const char*pxP[]={"Full","3/4","1/2","1/4",nullptr};
    Enumeration_knob(f,&_proxyMode,pxP,"proxy_mode","Proxy");
    Tooltip(f,"Downscale the render for faster interactive preview.\n"
              "Full = 100% resolution.\n"
              "3/4, 1/2, 1/4 = progressively lower resolution.\n"
              "Set back to Full before final render.");

    Divider(f,"3D Viewport");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Preview the volume in the 3D viewer for camera placement.<br>"
        "No camera required — works as soon as a VDB is loaded."
        "</font>");
    Bool_knob(f,&_showBbox,"show_bbox","Bounding Box");
    Tooltip(f,"Shows a green wireframe around the volume bounding box.\n"
              "Follows the Axis transform from the scene input.\n"
              "Useful for checking volume position and scale.");
    Bool_knob(f,&_showPoints,"show_points","Point Cloud");
    Tooltip(f,"Shows coloured dots representing density values.\n"
              "Updates when the volume data or transform changes.\n"
              "Useful for seeing the volume shape before rendering.");
    static const char*dL[]={"Low (~16k)","Medium (~64k)","High (~250k)",nullptr};
    Enumeration_knob(f,&_pointDensity,dL,"point_density","Point Density");
    Tooltip(f,"Number of sample points for the 3D preview cloud.\n"
              "Low = fast, good for positioning.\n"
              "High = detailed volume shape preview.");
    Double_knob(f,&_pointSize,"point_size","Point Size");SetRange(f,1,20);
    Tooltip(f,"OpenGL point size in pixels for the preview cloud.\n"
              "Increase on high-DPI displays for visibility.");
    Bool_knob(f,&_linkViewport,"link_viewport","Link Colour to Render Mode");
    Tooltip(f,"Viewport colour scheme follows the current render mode.\n"
              "Lit and Explosion modes use the Heat ramp for preview.");
    static const char*vp[]={"Greyscale","Heat","Cool","Blackbody","Custom Gradient","Explosion",nullptr};
    Enumeration_knob(f,&_viewportColor,vp,"viewport_color","Viewport Colour");
    Tooltip(f,"Override the viewport colour scheme manually.\n"
              "Only applies when 'Link Colour to Render Mode' is off.");
}

int VDBRenderIop::knob_changed(Knob* k)
{
    if(k->is("file")){_gridValid=false;_previewPoints.clear();return 1;}
    if(k->is("auto_sequence")&&_autoSequence){
        std::string p(_vdbFilePath?_vdbFilePath:"");
        if(!p.empty()){
            size_t dot=p.rfind(".vdb");if(dot==std::string::npos)dot=p.rfind(".VDB");
            if(dot!=std::string::npos){
                size_t end=dot;size_t start=end;
                while(start>0&&p[start-1]>='0'&&p[start-1]<='9')--start;
                if(start<end){
                    int ndig=(int)(end-start);if(ndig<4)ndig=4;
                    std::string padded(ndig,'#');
                    p=p.substr(0,start)+padded+p.substr(end);
                    knob("file")->set_text(p.c_str());
                }
            }
        }
        _gridValid=false;_previewPoints.clear();return 1;
    }
    if(k->is("grid_name")||k->is("temp_grid")||k->is("flame_grid")||k->is("vel_grid")||k->is("color_grid")||k->is("frame_offset")){
        _gridValid=false;_previewPoints.clear();return 1;}
    if(k->is("show_points")||k->is("point_density")){_previewPoints.clear();return 1;}
    if(k->is("discover_grids")){discoverGrids();return 1;}
    if(k->is("shutter_preset")&&_shutterPreset<3){
        static const double sOpen[]={0,-0.5,-1};static const double sClose[]={1,0.5,0};
        knob("shutter_open")->set_value(sOpen[_shutterPreset]);
        knob("shutter_close")->set_value(sClose[_shutterPreset]);return 1;
    }
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
        static const char*velN[]={"vel","v","velocity","motion",nullptr};
        static const char*colN[]={"Cd","color","colour","rgb","albedo",nullptr};
        auto match=[](const std::string&n,const char**l){for(int i=0;l[i];++i)if(n==l[i])return true;return false;};
        struct GI{std::string name,type,cat;};
        std::vector<GI> grids;std::string bestD,bestT,bestF,bestV,bestC;
        for(auto it=file.beginName();it!=file.endName();++it){
            std::string n=it.gridName();auto g=file.readGridMetadata(n);std::string ty=g->valueType();
            std::string cat="other";
            if(match(n,denN))cat="density";else if(match(n,tmpN))cat="temperature";
            else if(match(n,flmN))cat="flames";else if(match(n,velN))cat="velocity";
            else if(match(n,colN))cat="colour";
            grids.push_back({n,ty,cat});
            if(bestD.empty()&&cat=="density"&&ty=="float")bestD=n;
            if(bestT.empty()&&cat=="temperature"&&ty=="float")bestT=n;
            if(bestF.empty()&&cat=="flames"&&ty=="float")bestF=n;
            if(bestV.empty()&&cat=="velocity"&&(ty=="vec3s"||ty=="vec3f"))bestV=n;
            if(bestC.empty()&&cat=="colour"&&(ty=="vec3s"||ty=="vec3f"))bestC=n;
        }
        file.close();
        if(!bestD.empty())knob("grid_name")->set_text(bestD.c_str());
        if(!bestT.empty())knob("temp_grid")->set_text(bestT.c_str());
        if(!bestF.empty())knob("flame_grid")->set_text(bestF.c_str());
        if(!bestV.empty())knob("vel_grid")->set_text(bestV.c_str());
        if(!bestC.empty())knob("color_grid")->set_text(bestC.c_str());
        // Auto-set mode
        if(!bestT.empty()||!bestF.empty()){knob("color_scheme")->set_value(6);knob("emission_intensity")->set_value(2.0);}
        std::string msg;
        for(const auto&g:grids){msg+=g.name+" ("+g.type+")";if(g.cat!="other")msg+=" ["+g.cat+"]";msg+="\\n";}
        msg+="\\n";
        if(!bestD.empty())msg+="Density: "+bestD+"\\n";
        if(!bestT.empty())msg+="Temperature: "+bestT+"\\n";
        if(!bestF.empty())msg+="Flames: "+bestF+"\\n";
        if(!bestV.empty())msg+="Velocity: "+bestV+"\\n";
        if(!bestC.empty())msg+="Colour: "+bestC+"\\n";
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
    hash.append(_adaptiveStep);hash.append(_proxyMode);hash.append(_useFallbackLight);
    hash.append(_motionBlur);hash.append(_shutterOpen);hash.append(_shutterClose);hash.append(_motionSamples);
    hash.append(_aovDensity);hash.append(_aovEmission);hash.append(_aovShadow);hash.append(_aovDepth);
    for(int i=0;i<3;++i){hash.append(_lightDir[i]);hash.append(_lightColor[i]);}
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
    ChannelSet outChans=Mask_RGBA;
    // AOV channels — each AOV gets its own layer
    if(_aovDensity){outChans+=channel("vdb_density.red");outChans+=channel("vdb_density.green");outChans+=channel("vdb_density.blue");}
    if(_aovEmission){outChans+=channel("vdb_emission.red");outChans+=channel("vdb_emission.green");outChans+=channel("vdb_emission.blue");}
    if(_aovShadow){outChans+=channel("vdb_shadow.red");outChans+=channel("vdb_shadow.green");outChans+=channel("vdb_shadow.blue");}
    if(_aovDepth){outChans+=channel("vdb_depth.red");}
    info_.channels(outChans);info_.turn_on(outChans);
    set_out_channels(outChans);
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
    }
    // No early return — grid loading must proceed for viewport preview

    // Scene input (2): gather lights + axis from scene tree
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)_volFwd[i][j]=_volInv[i][j]=(i==j)?1:0;
    if(input(2)) gatherLights(Op::input(2));

    // Fallback light if none found in scene and checkbox is enabled
    if(_lights.empty()&&_useFallbackLight){CachedLight cl;
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
        _floatGrid.reset();_tempGrid.reset();_flameGrid.reset();_velGrid.reset();_colorGrid.reset();
        _gridValid=false;_hasTempGrid=false;_hasFlameGrid=false;_hasVelGrid=false;_hasColorGrid=false;_previewPoints.clear();
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
            // Velocity grid — Vec3 for motion blur
            std::string vgN(_velGridName?_velGridName:"");openvdb::GridBase::Ptr vbg;
            if(!vgN.empty()){for(auto it=file.beginName();it!=file.endName();++it)
                if(it.gridName()==vgN){vbg=file.readGrid(vgN);break;}}
            // Colour grid — Vec3 direct RGB
            std::string cgN(_colorGridName?_colorGridName:"");openvdb::GridBase::Ptr cbg;
            if(!cgN.empty()){for(auto it=file.beginName();it!=file.endName();++it)
                if(it.gridName()==cgN){cbg=file.readGrid(cgN);break;}}
            file.close();
            // Need at least one grid
            bool hasDensity=found&&bg&&bg->isType<openvdb::FloatGrid>();
            bool hasTemp=tbg&&tbg->isType<openvdb::FloatGrid>();
            bool hasFlame=fbg&&fbg->isType<openvdb::FloatGrid>();
            bool hasVel=vbg&&vbg->isType<openvdb::Vec3SGrid>();
            bool hasColor=cbg&&cbg->isType<openvdb::Vec3SGrid>();
            if(!hasDensity&&!hasTemp&&!hasFlame&&!hasColor){error("No valid grids found. Set grid names or use Discover Grids.");return;}
            if(hasDensity) _floatGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(bg);
            if(hasTemp){_tempGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(tbg);_hasTempGrid=true;}
            if(hasFlame){_flameGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(fbg);_hasFlameGrid=true;}
            if(hasVel){_velGrid=openvdb::gridPtrCast<openvdb::Vec3SGrid>(vbg);_hasVelGrid=true;}
            if(hasColor){_colorGrid=openvdb::gridPtrCast<openvdb::Vec3SGrid>(cbg);_hasColorGrid=true;}
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
    // Proxy scaling — map pixel coords to lower-res render
    static const double kProxyScales[]={1.0, 0.75, 0.5, 0.25};
    double pxScale=kProxyScales[std::clamp(_proxyMode,0,3)];

    // Read BG row if connected
    Row bgRow(x,r);
    bool hasBg=false;
    {Iop*bg=dynamic_cast<Iop*>(Op::input(0));
     if(bg){bg->get(y,x,r,channels,bgRow);hasBg=true;}}

    if(!_gridValid||!_camValid||(!_floatGrid&&!_tempGrid&&!_flameGrid&&!_hasColorGrid)){
        foreach(z,channels){float*p=row.writable(z);
            const float*bp=hasBg?bgRow[z]+x:nullptr;
            for(int i=x;i<r;++i)p[i]=bp?bp[i-x]:0;}
        return;}
    const Format&fmt=format();int W=fmt.width(),H=fmt.height();
    // Proxy: effective render resolution
    int pW=(int)(W*pxScale),pH=(int)(H*pxScale);if(pW<1)pW=1;if(pH<1)pH=1;
    double halfW=_halfW,halfH=_halfW*(double)H/(double)W;
    float*rO=row.writable(Chan_Red),*gO=row.writable(Chan_Green),*bO=row.writable(Chan_Blue),*aO=row.writable(Chan_Alpha);
    ColorScheme scheme=static_cast<ColorScheme>(_colorScheme);
    float gA[3]={(float)_gradStart[0],(float)_gradStart[1],(float)_gradStart[2]};
    float gB[3]={(float)_gradEnd[0],(float)_gradEnd[1],(float)_gradEnd[2]};

    // AOV channel pointers — density and shadow are greyscale RGB
    float*aovDenR=_aovDensity?row.writable(channel("vdb_density.red")):nullptr;
    float*aovDenG=_aovDensity?row.writable(channel("vdb_density.green")):nullptr;
    float*aovDenB=_aovDensity?row.writable(channel("vdb_density.blue")):nullptr;
    float*aovEmR=_aovEmission?row.writable(channel("vdb_emission.red")):nullptr;
    float*aovEmG=_aovEmission?row.writable(channel("vdb_emission.green")):nullptr;
    float*aovEmB=_aovEmission?row.writable(channel("vdb_emission.blue")):nullptr;
    float*aovShR=_aovShadow?row.writable(channel("vdb_shadow.red")):nullptr;
    float*aovShG=_aovShadow?row.writable(channel("vdb_shadow.green")):nullptr;
    float*aovShB=_aovShadow?row.writable(channel("vdb_shadow.blue")):nullptr;
    float*aovDpP=_aovDepth?row.writable(channel("vdb_depth.red")):nullptr;

    const float*bgR=hasBg?bgRow[Chan_Red]:nullptr;
    const float*bgG=hasBg?bgRow[Chan_Green]:nullptr;
    const float*bgB=hasBg?bgRow[Chan_Blue]:nullptr;
    const float*bgA=hasBg?bgRow[Chan_Alpha]:nullptr;

    // Motion blur time samples
    int nTimeSamples=(_motionBlur&&_hasVelGrid)?std::max(1,_motionSamples):1;
    double shutterSpan=_shutterClose-_shutterOpen;

    for(int ix=x;ix<r;++ix){
        // Proxy: map output pixel to effective resolution
        double px=(pxScale<1.0)?((int)(ix*pxScale)+0.5)/pxScale:ix+0.5;
        double py=(pxScale<1.0)?((int)(y*pxScale)+0.5)/pxScale:y+0.5;
        double ndcX=px/(double)W*2-1,ndcY=py/(double)H*2-1;
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

        // Accumulate across motion blur time samples
        float R=0,G=0,B=0,A=0;
        float aovDen=0,aovEmRv=0,aovEmGv=0,aovEmBv=0,aovSh=0,aovDp=0;

        for(int ts=0;ts<nTimeSamples;++ts){
            // Motion blur: offset ray origin by velocity * time
            openvdb::Vec3d mO=rayO;
            if(nTimeSamples>1&&_velGrid){
                double tNorm=(nTimeSamples>1)?(double)ts/(nTimeSamples-1):0.5;
                double tSample=_shutterOpen+tNorm*shutterSpan;
                // Sample velocity at ray origin
                auto iP=_velGrid->transform().worldToIndex(rayO);
                openvdb::Vec3s vel=openvdb::tools::BoxSampler::sample(_velGrid->getConstAccessor(),iP);
                mO=rayO+openvdb::Vec3d(vel[0],vel[1],vel[2])*tSample;
            }

            float sR=0,sG=0,sB=0,sA=0;
            if(scheme==kLit){marchRay(mO,rayD,sR,sG,sB,sA);sR*=ri;sG*=ri;sB*=ri;}
            else if(scheme==kExplosion){marchRayExplosion(mO,rayD,sR,sG,sB,sA);sR*=ri;sG*=ri;sB*=ri;}
            else{float den=0,alpha=0;marchRayDensity(mO,rayD,den,alpha);
                // Vec3 colour grid override
                if(_hasColorGrid&&_colorGrid&&alpha>1e-6f){
                    auto iPC=_colorGrid->transform().worldToIndex(mO);
                    openvdb::Vec3s cv=openvdb::tools::BoxSampler::sample(_colorGrid->getConstAccessor(),iPC);
                    sR=cv[0]*ri*alpha;sG=cv[1]*ri*alpha;sB=cv[2]*ri*alpha;sA=alpha;
                }else{
                    Color3 c=evalRamp(scheme,den,gA,gB,_tempMin,_tempMax);
                    sR=c.r*ri*alpha;sG=c.g*ri*alpha;sB=c.b*ri*alpha;sA=alpha;
                }
            }
            R+=sR;G+=sG;B+=sB;A+=sA;
        }

        // Average motion blur samples
        if(nTimeSamples>1){float inv=1.0f/nTimeSamples;R*=inv;G*=inv;B*=inv;A*=inv;}

        // Composite volume OVER background
        if(hasBg){
            float inv=1.0f-A;
            rO[ix]=R+bgR[ix]*inv;gO[ix]=G+bgG[ix]*inv;
            bO[ix]=B+bgB[ix]*inv;aO[ix]=A+bgA[ix]*inv;
        }else{
            rO[ix]=R;gO[ix]=G;bO[ix]=B;aO[ix]=A;
        }

        // AOV writes
        if(aovDenR){aovDenR[ix]=A;aovDenG[ix]=A;aovDenB[ix]=A;}
        if(aovEmR){aovEmR[ix]=R;aovEmG[ix]=G;aovEmB[ix]=B;}
        if(aovShR){float sh=1.0f-A;aovShR[ix]=sh;aovShG[ix]=sh;aovShB[ix]=sh;}
        if(aovDpP) aovDpP[ix]=0;
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
    double lastDen=0; // for adaptive stepping
    if(_volRI){
        VRI vri(*_volRI); // thread-safe shallow copy
        openvdb::math::Ray<double> wRay(origin,dir);
        if(!vri.setWorldRay(wRay)){outR=outG=outB=outA=0;return;}
        double it0,it1;
        while(vri.march(it0,it1)&&T>.001){
            auto wS=vri.getWorldPos(it0),wE=vri.getWorldPos(it1);
            double wT0=(wS-origin).dot(dir),wT1=(wE-origin).dot(dir);
            if(wT1<=0)continue;if(wT0<0)wT0=0;
            for(double t2=wT0;t2<wT1&&T>.001;){
                auto wP=origin+t2*dir;auto iP=xf.worldToIndex(wP);
                lastDen=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
                shadeSample(wP,iP);
                double curStep=step;
                if(_adaptiveStep){curStep=step*(lastDen<0.01?4.0:lastDen<0.1?2.0:1.0);}
                t2+=curStep;}
        }
    }else{
        // Fallback: manual AABB (shouldn't happen if grid is valid)
        double tEnter=0,tExit=1e9;
        for(int a=0;a<3;++a){double inv=(std::abs(dir[a])>1e-8)?1./dir[a]:1e38;
            double t0=(_bboxMin[a]-origin[a])*inv,t1=(_bboxMax[a]-origin[a])*inv;
            if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}
        if(tEnter>=tExit||tExit<=0)return;
        for(double t2=tEnter;t2<tExit&&T>.001;){
            auto wP=origin+t2*dir;auto iP=xf.worldToIndex(wP);
            lastDen=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
            shadeSample(wP,iP);
            double curStep=step;
            if(_adaptiveStep){curStep=step*(lastDen<0.01?4.0:lastDen<0.1?2.0:1.0);}
            t2+=curStep;}
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
            for(double t2=wT0;t2<wT1&&T>.001;){
                auto wP=origin+t2*dir;auto iP=xf.worldToIndex(wP);
                shadeSampleExplosion(wP,iP);
                double curStep=step;
                if(_adaptiveStep&&dAcc){float ld=openvdb::tools::BoxSampler::sample(*dAcc,iP)*(float)_densityMix;
                    curStep=step*(ld<0.01?4.0:ld<0.1?2.0:1.0);}
                t2+=curStep;}
        }
    }else{
        double tEnter=0,tExit=1e9;
        for(int a=0;a<3;++a){double inv=(std::abs(dir[a])>1e-8)?1./dir[a]:1e38;
            double t0=(_bboxMin[a]-origin[a])*inv,t1=(_bboxMax[a]-origin[a])*inv;
            if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}
        if(tEnter>=tExit||tExit<=0)return;
        for(double t2=tEnter;t2<tExit&&T>.001;){
            auto wP=origin+t2*dir;auto iP=xf.worldToIndex(wP);
            shadeSampleExplosion(wP,iP);
            double curStep=step;
            if(_adaptiveStep&&dAcc){float ld=openvdb::tools::BoxSampler::sample(*dAcc,iP)*(float)_densityMix;
                curStep=step*(ld<0.01?4.0:ld<0.1?2.0:1.0);}
            t2+=curStep;}
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

    auto shadeDensity=[&](const openvdb::Vec3d&iP)->float{
        float d=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
        if(d>1e-6f){double se=d*ext,ab=T*(1-std::exp(-se*step));
            float val=d;if(useTG){float tv=openvdb::tools::BoxSampler::sample(*tA,iP)*(float)_tempMix;
                val=(float)std::clamp((tv-_tempMin)/(_tempMax-_tempMin),0.,1.);}
            wV+=val*ab;T*=std::exp(-se*step);}
        return d;
    };

    if(_volRI){
        VRI vri(*_volRI);openvdb::math::Ray<double> wRay(origin,dir);
        if(!vri.setWorldRay(wRay))return;
        double it0,it1;
        while(vri.march(it0,it1)&&T>.001){
            auto wS=vri.getWorldPos(it0),wE=vri.getWorldPos(it1);
            double wT0=(wS-origin).dot(dir),wT1=(wE-origin).dot(dir);
            if(wT1<=0)continue;if(wT0<0)wT0=0;
            for(double t2=wT0;t2<wT1&&T>.001;){
                auto iP=xf.worldToIndex(origin+t2*dir);float ld=shadeDensity(iP);
                double curStep=step;
                if(_adaptiveStep){curStep=step*(ld<0.01?4.0:ld<0.1?2.0:1.0);}
                t2+=curStep;}
        }
    }else{
        double tEnter=0,tExit=1e9;
        for(int a=0;a<3;++a){double inv=(std::abs(dir[a])>1e-8)?1./dir[a]:1e38;
            double t0=(_bboxMin[a]-origin[a])*inv,t1=(_bboxMax[a]-origin[a])*inv;
            if(t0>t1)std::swap(t0,t1);tEnter=std::max(tEnter,t0);tExit=std::min(tExit,t1);}
        if(tEnter>=tExit||tExit<=0)return;
        for(double t2=tEnter;t2<tExit&&T>.001;){
            auto iP=xf.worldToIndex(origin+t2*dir);float ld=shadeDensity(iP);
            double curStep=step;
            if(_adaptiveStep){curStep=step*(ld<0.01?4.0:ld<0.1?2.0:1.0);}
            t2+=curStep;}
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
