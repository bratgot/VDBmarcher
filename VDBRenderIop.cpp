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
    "v3.1 — Dual-lobe HG, Mie, powder, analytical MS,\n"
    "HDDA shadows, transmittance cache, SH env lighting,\n"
    "ReSTIR sampling, procedural noise, deep output.\n\n"
#ifdef VDBRENDER_HAS_NEURAL
    "Supports .nvdb neural compressed volumes\n"
    "(10-100x smaller) alongside standard .vdb files.\n\n"
#endif
    "Inputs:\n"
    "  bg  (0) — Background plate (sets resolution)\n"
    "  cam (1) — Camera\n"
    "  scn (2) — Scene (lights, axis, transforms)\n\n"
    "Pipe lights, an Environment light, and an Axis\n"
    "into a Scene node, then connect to the scn input.\n"
    "All lights are auto-detected including EnvironLight\n"
    "nodes which provide the HDRI environment map.\n\n"
    "Created by Marten Blumen\n"
    "github.com/bratgot/VDBmarcher";

bool VDBRenderIop::_vdbInitialised = false;

// ═══ Blackbody / Ramps ═══

VDBRenderIop::Color3 VDBRenderIop::blackbody(double T) {
    // CIE 1931 chromaticity → linear sRGB
    // Valid over the full range used in rendering: 500 K (deep red) to 40000 K (blue-white arc)
    // Planckian locus approximation (Kang et al. 2002, extended):
    //   x(T) fitted piecewise, y from chromaticity diagram, then XYZ → sRGB

    if (T < 500)  T = 500;
    if (T > 40000) T = 40000;

    // Chromaticity x (Planckian locus, piecewise rational fit)
    double x;
    const double Ti = 1.0 / T;
    if (T <= 4000) {
        x = -0.2661239e9*(Ti*Ti*Ti) - 0.2343580e6*(Ti*Ti) + 0.8776956e3*Ti + 0.179910;
    } else {
        x = -3.0258469e9*(Ti*Ti*Ti) + 2.1070379e6*(Ti*Ti) + 0.2226347e3*Ti + 0.240390;
    }

    // Chromaticity y
    double y;
    if (T <= 2222) {
        y = -1.1063814*x*x*x - 1.34811020*x*x + 2.18555832*x - 0.20219683;
    } else if (T <= 4000) {
        y = -0.9549476*x*x*x - 1.37418593*x*x + 2.09137015*x - 0.16748867;
    } else {
        y =  3.0817580*x*x*x - 5.87338670*x*x + 3.75112997*x - 0.37001483;
    }

    // xyY → XYZ (normalised to Y=1)
    const double yy = (y > 1e-8) ? y : 1e-8;
    const double X  = x / yy;
    const double Z  = (1.0 - x - y) / yy;

    // XYZ → linear sRGB (D65 whitepoint)
    double r =  3.2406*X - 1.5372   - 0.4986*Z;
    double g = -0.9689*X + 1.8758   + 0.0415*Z;
    double b =  0.0557*X - 0.2040   + 1.0570*Z;

    // Clamp negative lobes (outside sRGB gamut at extreme temperatures)
    r = std::max(0.0, r);
    g = std::max(0.0, g);
    b = std::max(0.0, b);

    // Normalise so the channel with maximum value = 1, then scale by
    // perceptual brightness relative to 6500K reference
    const double peak = std::max({r, g, b, 1e-8});
    r /= peak; g /= peak; b /= peak;

    // Brightness: power-law relative to 6500K white (matches legacy behaviour)
    const double bri = std::clamp(std::pow(T / 6500.0, 0.4), 0.0, 2.0);

    return { float(r * bri), float(g * bri), float(b * bri) };
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
        " <font color='#888' size='-1'>v3.1</font><br>"
        "<font color='#aaa'>OpenVDB volume ray marcher"
#ifdef VDBRENDER_HAS_NEURAL
        " + NeuralVDB"
#endif
        "</font>");

    File_knob(f,&_vdbFilePath,"file","VDB File");
    Tooltip(f,"Path to your .vdb file from Houdini, Ember, or other VDB exporters.\n"
              "Sequences: use #### or %04d in the filename for frame padding.\n"
              "Tip: Enable 'Auto Sequence' to convert numbered files automatically.");
    Bool_knob(f,&_autoSequence,"auto_sequence","Auto Sequence");
    Tooltip(f,"Converts frame numbers in the filename to #### padding.\n"
              "e.g. explosion_0001.vdb becomes explosion_####.vdb\n"
              "and explosion_1.vdb becomes explosion_####.vdb\n"
              "Enable this if your VDB sequence isn't animating.");
    String_knob(f,&_origFilePath,"orig_file_path","");
    SetFlags(f,Knob::INVISIBLE);
    Format_knob(f,&_formats,"format","Format");
    Tooltip(f,"Output resolution when no background plate is connected.\n"
              "If a BG is connected to input 0, its format is used instead.");

    BeginGroup(f,"grp_grids","Grids");
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
              "Values in Kelvin drive fire colour (orange to white).");
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
    Divider(f,"");
    Text_knob(f,
        "<font size='-1' color='#557'>"
        "The following grids are in beta and may change in future releases."
        "</font>");
    String_knob(f,&_velGridName,"vel_grid","Velocity \xCE\xB2");
    Tooltip(f,"[Beta] Vec3 grid for motion blur.\n"
              "Common names: vel, v, velocity\n"
              "Enable motion blur in the Output tab to use this.\n\n"
              "Note: Velocity-based motion blur is experimental.\n"
              "Results may vary depending on simulation scale.");
    String_knob(f,&_colorGridName,"color_grid","Colour \xCE\xB2");
    Tooltip(f,"[Beta] Vec3 grid for direct RGB colour from your simulation.\n"
              "Common names: Cd, color, colour, rgb, albedo\n"
              "Overrides the render mode colour when loaded.\n\n"
              "Note: Colour grid support is experimental.\n"
              "Currently active in ramp modes only.");
    Int_knob(f,&_frameOffset,"frame_offset","Frame Offset");
    Tooltip(f,"Offsets the timeline frame for sequences.\n"
              "Use negative values to shift the sequence earlier.\n"
              "e.g. -10 makes frame 20 read file frame 10.");
    EndGroup(f);

    BeginGroup(f,"grp_render","Render");
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
    EndGroup(f);

    BeginClosedGroup(f,"grp_shading","Shading");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Extinction controls opacity. Scattering controls brightness under lighting.<br>"
        "Phase function and scatter quality are set in the Shading V2 tab."
        "</font>");
    Double_knob(f,&_extinction,"extinction","Extinction");SetRange(f,0,100);
    Tooltip(f,"How quickly light is absorbed per unit density.\n"
              "Higher = more opaque volume.\n"
              "Thin smoke: 1-5. Cloud: 10-30. Solid: 50+");
    Double_knob(f,&_scattering,"scattering","Scattering");SetRange(f,0,100);
    Tooltip(f,"How bright the volume appears under direct lighting.\n"
              "Only used in Lit and Explosion modes.\n"
              "0 = pure absorption (dark). Higher = brighter.");
    // [V2] anisotropy/aniso_preset replaced by g_forward/g_backward/lobe_mix in Shading V2 tab
    Double_knob(f,&_anisotropy,"anisotropy","");SetFlags(f,Knob::INVISIBLE|Knob::DO_NOT_WRITE);
    Int_knob(f,&_anisotropyPreset,"aniso_preset","");SetFlags(f,Knob::INVISIBLE|Knob::DO_NOT_WRITE);
    EndGroup(f);

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

    BeginClosedGroup(f,"grp_info","Technical Reference");
    Text_knob(f,
        "<font size='-1' color='#bbb'>"
        "<b>Radiative Transfer</b> — Beer-Lambert transmittance<br>"
        "with HDDA empty-space skipping and trilinear<br>"
        "BoxSampler interpolation."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Phase</b> — Dual-lobe Henyey-Greenstein (Shading V2).<br>"
        "<b>Scatter</b> — Analytical MS approximation (Wrenninge 2015).<br>"
        "<b>Powder</b> — Interior brightening (Schneider &amp; Vos 2015)."
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
    // ═══════════════════════════════════════════════════
    //  TAB: Shading V2
    // ═══════════════════════════════════════════════════
    Tab_knob(f,"Shading V2");

    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Dual-lobe HG phase function, powder effect, analytical<br>"
        "multiple scattering, chromatic extinction, and ray jitter.<br>"
        "All modes except Greyscale/Heat/Cool/Blackbody benefit."
        "</font>");

    BeginClosedGroup(f,"grp_phase_v2","Phase Function");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Dual-lobe HG is fast and versatile. Approximate Mie is physically<br>"
        "accurate for water droplets (clouds, fog) — Jendersie &amp; d'Eon 2023."
        "</font>");
    static const char*phaseM[]={"Dual-lobe HG","Approximate Mie",nullptr};
    Enumeration_knob(f,&_phaseMode,phaseM,"phase_mode","Phase Mode");
    Tooltip(f,"Dual-lobe HG — fast, versatile, good for all volume types.\n"
               "Approximate Mie — parametric fit to full Lorenz-Mie scattering.\n"
               "Physically accurate for spherical water droplets.\n"
               "Use for clouds and fog. Set Droplet Diameter below.");
    Double_knob(f,&_mieDropletD,"mie_droplet_d","Droplet Diameter"); SetRange(f,0.1,20);
    Tooltip(f,"Water droplet diameter in micrometres.\n"
               "Only used when Phase Mode = Approximate Mie.\n"
               "0.1 = aerosol / haze\n"
               "2.0 = typical cloud droplet\n"
               "10.0 = large cloud / light drizzle");
    Divider(f,"HG parameters (Dual-lobe mode)");
    Double_knob(f,&_gForward,  "g_forward",  "G Forward");  SetRange(f, 0, 1);
    Tooltip(f,"Forward-scatter lobe asymmetry (HG g1).\n"
               "0 = isotropic, 1 = fully forward.\n"
               "Smoke: 0.4   Cloud: 0.8   Explosion: 0.85");
    Double_knob(f,&_gBackward, "g_backward", "G Backward"); SetRange(f,-1, 0);
    Tooltip(f,"Backward-scatter lobe asymmetry (HG g2).\n"
               "0 = isotropic, -1 = fully backward (rim/halo).\n"
               "Smoke: -0.15   Cloud: -0.1   Explosion: -0.25");
    Double_knob(f,&_lobeMix,   "lobe_mix",   "Lobe Mix");   SetRange(f, 0, 1);
    Tooltip(f,"Blend between forward and backward lobes.\n"
               "1.0 = pure forward, 0.0 = pure backward.\n"
               "Typical range: 0.65-0.85");
    EndGroup(f);

    BeginClosedGroup(f,"grp_scatter_v2","Scatter Quality");
    Double_knob(f,&_powderStrength,"powder_strength","Powder Effect"); SetRange(f,0,10);
    Tooltip(f,"Interior brightening (Schneider & Vos 2015).\n"
               "0 = off. 2 = natural. 5 = dense explosion. 10 = maximum.\n"
               "Zero extra rays — free quality improvement.");
    Double_knob(f,&_gradientMix,"gradient_mix","Gradient Mix"); SetRange(f,0,1);
    Tooltip(f,"Blend HG phase with density-gradient Lambertian term.\n"
               "Gives clouds sculpted billowing edges.\n"
               "0 = off (best for smoke/fire). 0.3 = clouds.\n"
               "Adds 6 grid lookups per step when enabled.");
    Bool_knob(f,&_jitter,"jitter","Ray Jitter");
    Tooltip(f,"Per-pixel random step offset — eliminates wood-grain banding.\n"
               "Zero render cost. Leave on at all quality levels.");
    EndGroup(f);

    BeginClosedGroup(f,"grp_ms_v2","Multiple Scatter (Analytical)");
    Bool_knob(f,&_msApprox,"ms_approx","Analytical MS");
    Tooltip(f,"Wrenninge 2015 analytical approximation to infinite-bounce scattering.\n"
               "Replaces brute-force bounce rays at 100x lower cost.\n"
               "Leave ON — disable only to compare against legacy bounces.");
    Color_knob(f,_msTint,"ms_tint","Scatter Tint");
    Tooltip(f,"Colour tint on the multiple-scatter contribution.\n"
               "Default (1.0, 0.97, 0.95) is a slight warm bias.\n"
               "Try (0.95, 0.97, 1.0) for cold/overcast scenes.");
    EndGroup(f);

    BeginClosedGroup(f,"grp_chroma_v2","Chromatic Extinction");
    Bool_knob(f,&_chromaticExt,"chromatic_ext","Enable Chromatic");
    Tooltip(f,"Separate extinction per RGB channel.\n"
               "Off: all channels use the main Extinction value.\n"
               "On: use the three sliders below.");
    Double_knob(f,&_extR,"ext_r","Extinction R"); SetRange(f,0,100);
    Tooltip(f,"Red-channel extinction. Lower than B = warm depth shift.");
    Double_knob(f,&_extG,"ext_g","Extinction G"); SetRange(f,0,100);
    Double_knob(f,&_extB,"ext_b","Extinction B"); SetRange(f,0,100);
    Tooltip(f,"Blue-channel extinction. Higher than R = blue scatters more (smoke).");
    EndGroup(f);

    Divider(f,"");
    Text_knob(f,
        "<font size='-1' color='#666'>"
        "Schneider &amp; Vos (HZD 2015) · Wrenninge+ (SIGGRAPH 2017)<br>"
        "Jendersie &amp; d'Eon (SIGGRAPH 2023)"
        "</font>");

    BeginClosedGroup(f,"grp_noise_v4","Procedural Detail Noise");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "fBm noise perturbs density at render time, adding micro-detail<br>"
        "beyond VDB resolution. World-space — no UV unwrap needed.<br>"
        "Cost: one noise eval per march step when enabled."
        "</font>");
    Bool_knob(f,&_noiseEnable,"noise_enable","Enable Noise");
    Tooltip(f,"Add procedural fBm detail noise to the density at render time.\n"
               "Raises apparent VDB resolution without resampling.\n"
               "Useful for wispy smoke edges and cloud surface detail.");
    Double_knob(f,&_noiseScale,"noise_scale","Scale"); SetRange(f,0.1,20);
    Tooltip(f,"World-space frequency of the noise relative to the volume bbox.\n"
               "1 = one full noise cycle across the bbox.\n"
               "4 = fine detail. 8 = very fine. 0.5 = large lumps.");
    Double_knob(f,&_noiseStrength,"noise_strength","Strength"); SetRange(f,0,1);
    Tooltip(f,"How strongly the noise modulates density.\n"
               "0 = no effect. 0.5 = ±50% density variation. 1 = ±100%.\n"
               "Start at 0.3-0.5 for natural-looking detail.");
    Int_knob(f,&_noiseOctaves,"noise_octaves","Octaves"); SetRange(f,1,6);
    Tooltip(f,"Number of fBm octave layers.\n"
               "1 = smooth, large-scale variation.\n"
               "3 = natural. 6 = very detailed but slower.");
    Double_knob(f,&_noiseRoughness,"noise_roughness","Roughness"); SetRange(f,0,1);
    Tooltip(f,"Amplitude falloff per octave.\n"
               "0 = only the base octave contributes (smooth).\n"
               "0.5 = natural. 1 = all octaves equal weight (very rough).");
    EndGroup(f);

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
    //  TAB: Lighting
    // ═══════════════════════════════════════════════════
    Tab_knob(f,"Lighting");

    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Sun/Sky and Studio lights compose with scene lights from the scn input.<br>"
        "All sources are additive — use the Mix sliders to balance them.<br>"
        "Set Sky Mix and Studio Mix to 0 to use scene lights only."
        "</font>");

    Divider(f,"Sun and Sky");
    static const char*skyP[]={"Off","Custom","Day Sky","Golden Hour","Overcast","Blue Hour","Night / Moon",nullptr};
    Enumeration_knob(f,&_skyPreset,skyP,"sky_preset","Sky Preset");
    Tooltip(f,"Sets sun/sky sliders for common lighting conditions.\n"
              "Off — disables sun and sky entirely\n"
              "Custom — adjust sliders manually\n"
              "Day Sky — noon sun, clear blue sky\n"
              "Golden Hour — low warm sunset light\n"
              "Overcast — soft cloudy dome, no direct sun\n"
              "Blue Hour — cool post-sunset twilight\n"
              "Night / Moon — dim cool moonlight");
    Double_knob(f,&_skyMix,"sky_mix","Sky Mix");SetRange(f,0,2);
    Tooltip(f,"Master brightness for all sun and sky lights.\n"
              "0 = off. 1 = natural. 2 = double brightness.\n"
              "Use this to balance sun/sky against studio lights.");

    BeginClosedGroup(f,"grp_sky_detail","Sun and Sky Settings");
    Double_knob(f,&_sunElevation,"sun_elevation","Sun Elevation");SetRange(f,0,90);
    Tooltip(f,"Sun height above the horizon in degrees.\n"
              "90 = directly overhead (noon)\n"
              "30 = late afternoon\n"
              "10 = sunset, warm colours\n"
              "0 = at the horizon");
    Double_knob(f,&_sunAzimuth,"sun_azimuth","Sun Azimuth");SetRange(f,0,360);
    Tooltip(f,"Sun horizontal angle in degrees.\n"
              "0/360 = North, 90 = East, 180 = South, 270 = West.");
    Double_knob(f,&_sunIntensity,"sun_intensity","Sun Intensity");SetRange(f,0,20);
    Tooltip(f,"Brightness of the direct sunlight.\n"
              "3 = natural daylight. Higher for dramatic contrast.");
    Double_knob(f,&_skyIntensity,"sky_intensity","Sky Fill");SetRange(f,0,5);
    Tooltip(f,"Brightness of the sky dome fill light.\n"
              "0.3-0.5 = natural outdoor. Higher for softer shadows.");
    Double_knob(f,&_turbidity,"turbidity","Turbidity");SetRange(f,2,10);
    Tooltip(f,"Atmospheric haziness. Affects sun and sky colour.\n"
              "2 = crystal clear. 3 = clear day. 6 = hazy. 10 = heavy.");
    Double_knob(f,&_groundBounce,"ground_bounce","Ground Bounce");SetRange(f,0,1);
    Tooltip(f,"Light bounced up from the ground surface.\n"
              "Fills the underside of volumes.\n"
              "0 = none. 0.1 = subtle. 0.3 = sandy ground.");
    EndGroup(f);

    Divider(f,"Studio Lights");
    static const char*stuP[]={"Off","3-Point","Dramatic","Soft","Rim Only","Top Light",nullptr};
    Enumeration_knob(f,&_studioPreset,stuP,"studio_preset","Studio Preset");
    Tooltip(f,"Sets studio light sliders for common setups.\n"
              "3-Point — key + fill + rim (standard)\n"
              "Dramatic — strong key, minimal fill, hard rim\n"
              "Soft — broad wrap, gentle rim (beauty)\n"
              "Rim Only — backlight silhouette\n"
              "Top Light — overhead, moody\n"
              "Choose Off to disable studio lights.");
    Double_knob(f,&_studioMix,"studio_mix","Studio Mix");SetRange(f,0,2);
    Tooltip(f,"Master brightness for all studio lights.\n"
              "0 = off. 1 = natural. 2 = double brightness.\n"
              "Blend with Sky Mix for hybrid lighting.");

    BeginClosedGroup(f,"grp_studio_detail","Studio Light Settings");
    Double_knob(f,&_studioKeyAzimuth,"studio_key_azimuth","Key Azimuth");SetRange(f,0,360);
    Tooltip(f,"Horizontal angle of the key (main) light.\n"
              "45 = classic 3/4 from camera-left. 315 = camera-right.");
    Double_knob(f,&_studioKeyElevation,"studio_key_elevation","Key Elevation");SetRange(f,0,90);
    Tooltip(f,"Height of the key light.\n"
              "40 = standard portrait. 70 = high, dramatic shadows.");
    Double_knob(f,&_studioKeyIntensity,"studio_key_intensity","Key Intensity");SetRange(f,0,20);
    Tooltip(f,"Brightness of the main studio light.");
    Double_knob(f,&_studioFillRatio,"studio_fill_ratio","Fill Ratio");SetRange(f,0,1);
    Tooltip(f,"Fill light as a fraction of key intensity.\n"
              "0 = no fill (very dramatic). 0.5 = soft (beauty).\n"
              "Fill comes from the opposite side of the key.");
    Double_knob(f,&_studioRimIntensity,"studio_rim_intensity","Rim Intensity");SetRange(f,0,20);
    Tooltip(f,"Backlight intensity for edge definition.\n"
              "0 = no rim. 2 = standard. 4 = strong halo.");
    EndGroup(f);

    Divider(f,"Global");
    Double_knob(f,&_ambientIntensity,"ambient","Ambient");SetRange(f,0,5);
    Tooltip(f,"Omnidirectional fill. Lifts the darkest areas.\n"
              "Use sparingly — too much flattens the volume.");

    BeginClosedGroup(f,"grp_env","Environment Map");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "Picked up from EnvironLight in the scene tree."
        "</font>");
    Double_knob(f,&_envIntensity,"env_intensity","Env Intensity");SetRange(f,0,10);
    Double_knob(f,&_envRotate,"env_rotate","Rotate Offset");SetRange(f,0,360);
    Double_knob(f,&_envDiffuse,"env_diffuse","Diffuse");SetRange(f,0,1);
    Divider(f,"");
    static const char*envM[]={"Uniform dirs (slow, accurate)","SH + Virtual Lights (fast)",nullptr};
    Enumeration_knob(f,&_envMode,envM,"env_mode","Env Mode");
    Tooltip(f,"SH + Virtual Lights (recommended): projects the env map to 9 SH\n"
               "coefficients at load time. Full-sphere ambient evaluated in 9\n"
               "multiply-adds per march step — no shadow rays for ambient.\n"
               "Brightest env peaks extracted as virtual directional lights\n"
               "with proper HDDA shadow rays. 10-70x faster than Uniform dirs.\n\n"
               "Uniform dirs: original method — 6-26 directions × shadow steps.\n"
               "Use to compare quality or when exact directionality matters.");
    Int_knob(f,&_envVirtualLights,"env_virtual_lights","Virtual Lights");SetRange(f,0,4);
    Tooltip(f,"Number of bright peaks extracted from the env map as virtual\n"
               "directional lights. Only used in SH + Virtual Lights mode.\n"
               "0 = SH ambient only (fastest, no directional self-shadowing).\n"
               "1 = sun only. 2 = sun + bright sky region (recommended).\n"
               "Virtual lights benefit from the V3 transmittance cache.");
    Bool_knob(f,&_useReSTIR,"use_restir","ReSTIR Env Sampling");
    Tooltip(f,"Weighted reservoir importance sampling for environment lighting.\n"
               "Samples all 26 directions by SH radiance × phase weight,\n"
               "selects the best candidate, traces one shadow ray per step.\n"
               "Equal or better quality to uniform dirs at ~26x fewer shadow rays.\n"
               "Best combined with Shadow Cache for near-zero env shadow cost.\n"
               "Only active in Uniform dirs mode.");
    EndGroup(f);

    BeginClosedGroup(f,"grp_manual","Custom Fill Light");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "A single directional light added on top of the scene and rig lights.<br>"
        "Useful for a quick fill, bounce, or rim when no scene input is connected,<br>"
        "or to add a complementary light to an existing setup."
        "</font>");

    Bool_knob(f,&_useFallbackLight,"use_fallback_light","Enable");
    Tooltip(f,"Enable the custom directional fill light.\n\n"
              "This light is always additive — it adds to whatever lights\n"
              "already exist from the scene input and lighting rig.\n"
              "It is NOT a fallback that replaces missing lights.\n\n"
              "Common uses:\n"
              "  · Quick fill when no scene lights are connected\n"
              "  · Rim or back-light to define volume silhouettes\n"
              "  · Bounce light from a surface below the volume\n"
              "  · Secondary key for artistic contrast\n\n"
              "The light participates fully in shadow rays,\n"
              "phase function evaluation, and multiple scattering.");

    Divider(f,"");

    XYZ_knob(f,_lightDir,"light_dir","Direction");
    Tooltip(f,"Direction pointing TOWARD the light source.\n\n"
              "This is the vector from the volume to the light —\n"
              "the same direction shadow rays are cast.\n\n"
              "  (0,  1,  0) = light from above (overhead sun)\n"
              "  (0, -1,  0) = light from below (ground bounce)\n"
              "  (1,  1, -1) = light from upper-right-front (key)\n"
              " (-1,  1,  0) = light from upper-left (fill)\n\n"
              "The viewport handle shows the disc where the light\n"
              "source is, with an arrow pointing toward the volume.\n"
              "The vector does not need to be normalised.");

    Color_knob(f,_lightColor,"light_color","Tint");
    Tooltip(f,"Colour tint multiplied by the Intensity value.\n\n"
              "Common choices:\n"
              "  White (1,1,1)   — neutral fill, adds brightness evenly\n"
              "  Warm (1,0.9,0.7) — afternoon bounce or warm key\n"
              "  Cool (0.7,0.85,1) — sky fill, shadow fill, moonlight\n"
              "  Coloured        — gelled light for atmospheric effects\n\n"
              "This is a linear RGB colour — values above 1.0 are valid\n"
              "and will boost that channel beyond white.");

    Double_knob(f,&_lightIntensity,"light_intensity","Intensity");SetRange(f,0,20);
    Tooltip(f,"Brightness of the custom fill light.\n\n"
              "Relative to scene lights — a value of 1.0 is roughly\n"
              "equivalent to a standard directional light at full intensity.\n\n"
              "Suggested starting points:\n"
              "  0.1 – 0.3   Subtle fill, barely visible, lifts shadow areas\n"
              "  0.5 – 1.0   Moderate fill or secondary key\n"
              "  1.0 – 3.0   Strong key or artistic emphasis\n"
              "  3.0+        Overexposed / stylised look\n\n"
              "Tip: for a fill light, keep it below the main key intensity\n"
              "to preserve the volume's sense of depth and self-shadowing.");

    EndGroup(f);

    BeginClosedGroup(f,"grp_light_info","Lighting Technical Reference");
    Text_knob(f,
        "<font size='-1' color='#bbb'>"
        "<b>Sun and Sky Model</b><br>"
        "Simplified analytical sky based on Preetham et al. (1999).<br>"
        "Sun colour is derived from elevation and turbidity: high<br>"
        "elevation produces white light, low elevation shifts through<br>"
        "gold, orange, and red via Planckian-like chromaticity. Sky<br>"
        "dome colour follows Rayleigh scattering — deep blue at noon,<br>"
        "warm purple at sunset. Turbidity controls Mie scattering<br>"
        "from aerosols: higher values add haze warmth, desaturate<br>"
        "the sky, and reduce blue. Ground bounce approximates<br>"
        "Lambertian diffuse reflection from the terrain surface."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Studio Lighting</b><br>"
        "Three-point lighting follows cinematographic convention:<br>"
        "the Key light provides primary illumination with slight<br>"
        "warmth (Kelvin ~5500K). The Fill light sits opposite the<br>"
        "key at half elevation with a cool tint, controlled by the<br>"
        "Fill Ratio as a fraction of key intensity. The Rim (back)<br>"
        "light is placed behind and above the subject to define<br>"
        "edge silhouettes. Dramatic mode reduces fill to ~8% for<br>"
        "high-contrast chiaroscuro. Soft mode uses two flanking<br>"
        "keys offset +/-30 degrees for broad wrap-around lighting."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Mix Controls</b><br>"
        "Sky Mix and Studio Mix are master multipliers applied to<br>"
        "all lights in their respective group. Both can be active<br>"
        "simultaneously for hybrid lighting — e.g. outdoor sun with<br>"
        "a subtle studio rim for edge definition. Values above 1.0<br>"
        "boost beyond natural intensity for artistic emphasis."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Light Generation</b><br>"
        "All rig lights are generated as directional CachedLight<br>"
        "entries during validate — identical to scene lights from<br>"
        "the scn input. They participate in the same shadow ray,<br>"
        "phase function, and multi-scatter calculations. Scene<br>"
        "lights always override the rig entirely."
        "</font><br><br>"
        "<font size='-1' color='#888'>"
        "Preetham, Shirley, Smits (1999) — A Practical Analytic<br>"
        "Model for Daylight, SIGGRAPH 99<br>"
        "Henyey-Greenstein (1941) — Phase function for volume scatter"
        "</font>");
    EndGroup(f);

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

    Divider(f,"Sampling");
    Int_knob(f,&_renderSamples,"render_samples","Render Samples");SetRange(f,1,16);
    Tooltip(f,"Number of independent stochastic passes per pixel, averaged.\n"
              "Each pass uses a different jitter seed — noise reduces as sqrt(N).\n"
              "1 = single pass (default, fastest).\n"
              "4 = 2x noise reduction. 9 = 3x. 16 = 4x.\n"
              "Cost scales linearly. Use 4-9 for final renders,\n"
              "1-2 for interactive work.");

    // [V2] brute-force scatter replaced by analytical MS in Shading V2 tab
    Int_knob(f,&_scatterPreset,"scatter_preset","");SetFlags(f,Knob::INVISIBLE|Knob::DO_NOT_WRITE);
    Int_knob(f,&_multiBounces,"multi_bounces","");SetFlags(f,Knob::INVISIBLE|Knob::DO_NOT_WRITE);
    Int_knob(f,&_bounceRays,  "bounce_rays",  "");SetFlags(f,Knob::INVISIBLE|Knob::DO_NOT_WRITE);

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

    Divider(f,"Shadow Performance");
    Text_knob(f,
        "<font size='-1' color='#777'>"
        "HDDA empty-space skip is always active on shadow rays (free).<br>"
        "Shadow Cache precomputes transmittance per directional light,<br>"
        "reducing shadow cost from O(steps) to O(1) per primary sample."
        "</font>");
    Bool_knob(f,&_useShadowCache,"shadow_cache","Shadow Cache");
    Tooltip(f,"Precomputes a transmittance grid per directional light.\n"
              "Shadow rays replaced by a single trilinear lookup.\n"
              "5-10x faster shadow evaluation for directional lights.\n"
              "Rebuilt automatically when lights or VDB file changes.\n"
              "Point lights always use HDDA shadow rays.");
    static const char*cRes[]={"Full (1:1)","Half (2:1)","Quarter (4:1)",nullptr};
    Enumeration_knob(f,&_shadowCacheRes,cRes,"shadow_cache_res","Cache Resolution");
    Tooltip(f,"Voxel resolution of the precomputed transmittance grid.\n"
              "Full = same resolution as density grid, best quality.\n"
              "Half = recommended for production, 8x less memory.\n"
              "Quarter = fastest build, slight shadow softening.");

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

#ifdef VDBRENDER_HAS_NEURAL
    // ═══════════════════════════════════════════════════
    //  TAB: Neural
    // ═══════════════════════════════════════════════════
    Tab_knob(f,"Neural");

    Text_knob(f,
        "<font size='-1' color='#777'>"
        "NeuralVDB compresses VDB files 10-100x using neural networks.<br>"
        "Load a .nvdb file (created by nvdb_encoder.py) and all existing<br>"
        "render modes, lighting, and deep output work automatically."
        "</font>");

    Divider(f,"Neural Decode");

    Bool_knob(f,&_neuralUseCuda,"neural_cuda","CUDA Inference");
    Tooltip(f,"Use GPU for neural network inference.\n"
              "Requires CUDA-compatible GPU and LibTorch+CUDA build.\n"
              "Falls back to CPU if unavailable.");

    Bool_knob(f,&_neuralDecodeToGrid,"neural_decode_grid","Decode to Grid");
    Tooltip(f,"DEFAULT ON (recommended). Decode all voxels into a\n"
              "standard VDB grid at load time. Rendering then uses\n"
              "BoxSampler at full speed — zero neural overhead.\n\n"
              "Turn OFF for live neural inference per sample (slower\n"
              "rendering but uses less memory — the decoded grid\n"
              "doesn't need to fit in RAM alongside the original).");
    SetFlags(f,Knob::STARTLINE);

    Int_knob(f,&_neuralBatchSize,"neural_batch_size","Batch Size");
    Tooltip(f,"Number of voxels per neural network forward pass.\n"
              "Larger = faster decode, more GPU memory.\n"
              "4096 is a good default. Try 16384 for large volumes on GPU.");

    String_knob(f,&_neuralInfoRatio,"neural_info_ratio","Compression");
    SetFlags(f,Knob::READ_ONLY|Knob::DO_NOT_WRITE);

    String_knob(f,&_neuralInfoPSNR,"neural_info_psnr","PSNR");
    SetFlags(f,Knob::READ_ONLY|Knob::DO_NOT_WRITE);

    String_knob(f,&_neuralInfoMode,"neural_info_mode","Decode Mode");
    SetFlags(f,Knob::READ_ONLY|Knob::DO_NOT_WRITE);

    BeginClosedGroup(f,"grp_neural_info","Neural Technical Reference");
    Text_knob(f,
        "<font size='-1' color='#bbb'>"
        "<b>NeuralVDB</b> (Kim et al. 2024) replaces lower VDB tree<br>"
        "nodes with compact neural networks. The upper tree provides<br>"
        "HDDA empty-space skipping. Two MLPs per volume:<br>"
        "1. Topology classifier — predicts active voxel mask<br>"
        "2. Value regressor — predicts density at active positions<br>"
        "Both use Fourier positional encoding for detail."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Sampling</b> — Neural decode replaces BoxSampler at the<br>"
        "leaf level. All lighting, shadows, phase function, multi-<br>"
        "scatter, deep output, and AOVs work identically."
        "</font><br><br>"
        "<font size='-1' color='#888'>"
        "Kim, Museth et al. (2024) — NeuralVDB, ACM TOG"
        "</font>");
    EndGroup(f);
#endif
}

int VDBRenderIop::knob_changed(Knob* k)
{
    if(k->is("file")){_gridValid=false;_previewPoints.clear();return 1;}
    if(k->is("auto_sequence")){
        if(_autoSequence){
            // Store original path in hidden knob, then convert to ####
            std::string p(_vdbFilePath?_vdbFilePath:"");
            knob("orig_file_path")->set_text(p.c_str());
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
        }else{
            // Restore original path from hidden knob
            const char* orig=_origFilePath?_origFilePath:"";
            if(orig[0]){
                knob("file")->set_text(orig);
                knob("orig_file_path")->set_text("");
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
       struct Preset {
            int mode; double ext,scat; int shSteps;
            double shDen,tMin,tMax,emInt,flInt,quality,ambient;
            double intensity,envDiff;
            int skyP,stuP;
            // [V2] phase function presets
            double gFwd,gBck,lobeMix,powder;
            // [V4] gradient mix (0=off, 0.3=clouds)
            double gradMix;
        };
        //                              mode ext    scat   sh  shDen tMn   tMx    eI   fI   q    amb  int  envD  skyP stuP  gFwd  gBck  mix  pwd  grad
        static const Preset pv[] = {
            {},                         // 0: Custom
            {0, 2.0,  1.5,  8, 1.0,    500,6500,  0,   0,   3, 0.1,  1.0, 0.5,  2, 0,  0.40,-0.15,0.80,1.5, 0.0}, // 1: Thin Smoke
            {0, 15.0, 6.0, 12, 1.0,    500,6500,  0,   0,   3, 0.05, 1.0, 0.5,  2, 0,  0.50,-0.15,0.75,2.0, 0.0}, // 2: Dense Smoke
            {0, 1.0,  0.95, 8, 0.5,    500,6500,  0,   0,   3, 0.3,  1.0, 0.8,  4, 0,  0.80,-0.10,0.85,1.5, 0.0}, // 3: Fog / Mist
            {0, 12.0,11.4, 16, 1.0,    500,6500,  0,   0,   5, 0.2,  1.0, 0.6,  2, 0,  0.80,-0.10,0.80,3.0, 0.3}, // 4: Cumulus Cloud — gradMix=0.3
            {6, 5.0,  2.0,  8, 0.6,    800,3000,  2.5, 5.0, 3, 0.15, 1.5, 0.3,  3, 0,  0.85,-0.25,0.65,3.0, 0.0}, // 5: Fire
            {6, 20.0, 5.0, 16, 0.5,    500,6000,  2.0, 3.5, 5, 0.1,  1.0, 0.4,  2, 0,  0.85,-0.25,0.65,5.0, 0.0}, // 6: Explosion
            {6, 30.0, 6.0, 16, 0.4,   1000,8000,  1.5, 2.5, 7, 0.08, 0.8, 0.3,  2, 0,  0.60,-0.20,0.70,4.0, 0.0}, // 7: Pyroclastic
            {0, 4.0,  3.0,  8, 1.0,    500,6500,  0,   0,   3, 0.15, 1.0, 0.5,  2, 0,  0.50,-0.30,0.60,2.0, 0.0}, // 8: Dust Storm
            {0, 2.0,  1.9,  8, 0.5,    500,6500,  0,   0,   3, 0.2,  1.0, 0.7,  0, 3,  0.70,-0.10,0.80,1.5, 0.0}, // 9: Steam
        };
        const auto&p=pv[_scenePreset];
        knob("color_scheme")->set_value(p.mode);_colorScheme=p.mode;
        knob("extinction")->set_value(p.ext);_extinction=p.ext;
        knob("scattering")->set_value(p.scat);_scattering=p.scat;
        knob("shadow_steps")->set_value(p.shSteps);_shadowSteps=p.shSteps;
        knob("shadow_density")->set_value(p.shDen);_shadowDensity=p.shDen;
        knob("temp_min")->set_value(p.tMin);_tempMin=p.tMin;
        knob("temp_max")->set_value(p.tMax);_tempMax=p.tMax;
        knob("emission_intensity")->set_value(p.emInt);_emissionIntensity=p.emInt;
        knob("flame_intensity")->set_value(p.flInt);_flameIntensity=p.flInt;
        knob("quality")->set_value(p.quality);_quality=p.quality;
        knob("ambient")->set_value(p.ambient);_ambientIntensity=p.ambient;
        knob("ramp_intensity")->set_value(p.intensity);_rampIntensity=p.intensity;
        knob("env_diffuse")->set_value(p.envDiff);_envDiffuse=p.envDiff;
        knob("sky_preset")->set_value(p.skyP);_skyPreset=p.skyP;
        knob("studio_preset")->set_value(p.stuP);_studioPreset=p.stuP;
        if(p.skyP>0){knob("sky_mix")->set_value(1.0);_skyMix=1.0;
            knob("studio_mix")->set_value(0.0);_studioMix=0.0;}
        if(p.stuP>0){knob("studio_mix")->set_value(1.0);_studioMix=1.0;
            if(p.skyP==0){knob("sky_mix")->set_value(0.0);_skyMix=0.0;}}
        // [V2] set dual-lobe phase function and powder from preset
        knob("g_forward") ->set_value(p.gFwd);  _gForward  = p.gFwd;
        knob("g_backward")->set_value(p.gBck);  _gBackward = p.gBck;
        knob("lobe_mix")  ->set_value(p.lobeMix);_lobeMix  = p.lobeMix;
        knob("powder_strength")->set_value(p.powder);_powderStrength=p.powder;
        knob("gradient_mix")->set_value(p.gradMix);_gradientMix=p.gradMix;
        knob("quality_preset")->set_value(0);_qualityPreset=0;
        return 1;
    }
    if(k->is("sky_preset")){
        if(_skyPreset==0){
            // Off — zero everything
            knob("sky_mix")->set_value(0);_skyMix=0;
            knob("sun_intensity")->set_value(0);_sunIntensity=0;
            knob("sky_intensity")->set_value(0);_skyIntensity=0;
            knob("ground_bounce")->set_value(0);_groundBounce=0;
            return 1;
        }
        if(_skyPreset==1) return 1; // Custom — leave sliders as-is
        //                  elev  azim  sunI  skyI  turb  grnd  mix
        struct SkyP{double e,a,si,ski,t,gb,m;};
        static const SkyP sp[]={
            {},                                    // padding for index 0,1
            {},
            { 45, 180, 3.0, 0.4, 3.0, 0.1, 1.0},  // 2: Day Sky
            {  8, 220, 2.5, 0.15,4.0, 0.05,1.0},  // 3: Golden Hour
            { 30, 180, 0.0, 0.9, 6.0, 0.05,1.0},  // 4: Overcast
            {  3, 250, 0.3, 0.15,3.0, 0.02,1.0},  // 5: Blue Hour
            { 35, 120, 0.4, 0.03,2.5, 0.0, 1.0},  // 6: Night / Moon
        };
        if(_skyPreset>=2&&_skyPreset<(int)(sizeof(sp)/sizeof(SkyP))){
            const auto&s=sp[_skyPreset];
            knob("sun_elevation")->set_value(s.e);_sunElevation=s.e;
            knob("sun_azimuth")->set_value(s.a);_sunAzimuth=s.a;
            knob("sun_intensity")->set_value(s.si);_sunIntensity=s.si;
            knob("sky_intensity")->set_value(s.ski);_skyIntensity=s.ski;
            knob("turbidity")->set_value(s.t);_turbidity=s.t;
            knob("ground_bounce")->set_value(s.gb);_groundBounce=s.gb;
            knob("sky_mix")->set_value(s.m);_skyMix=s.m;
        }
        return 1;
    }
    if(k->is("studio_preset")){
        if(_studioPreset==0){
            // Off — zero mix
            knob("studio_mix")->set_value(0);_studioMix=0;
            knob("studio_key_intensity")->set_value(0);_studioKeyIntensity=0;
            knob("studio_rim_intensity")->set_value(0);_studioRimIntensity=0;
            return 1;
        }
        //                keyAz  keyEl keyI  fill  rimI  mix
        struct StuP{double ka,ke,ki,fr,ri,m;};
        static const StuP stp[]={
            {},                                // 0: Off (handled above)
            { 45, 40, 3.0, 0.35, 2.0, 1.0},   // 1: 3-Point
            { 45, 35, 4.0, 0.08, 3.0, 1.0},   // 2: Dramatic
            { 30, 45, 2.0, 0.50, 1.2, 1.0},   // 3: Soft
            {180, 30, 0.0, 0.0,  4.0, 1.0},   // 4: Rim Only
            {  0, 80, 3.5, 0.15, 0.5, 1.0},   // 5: Top Light
        };
        if(_studioPreset>=1&&_studioPreset<(int)(sizeof(stp)/sizeof(StuP))){
            const auto&s=stp[_studioPreset];
            knob("studio_key_azimuth")->set_value(s.ka);_studioKeyAzimuth=s.ka;
            knob("studio_key_elevation")->set_value(s.ke);_studioKeyElevation=s.ke;
            knob("studio_key_intensity")->set_value(s.ki);_studioKeyIntensity=s.ki;
            knob("studio_fill_ratio")->set_value(s.fr);_studioFillRatio=s.fr;
            knob("studio_rim_intensity")->set_value(s.ri);_studioRimIntensity=s.ri;
            knob("studio_mix")->set_value(s.m);_studioMix=s.m;
        }
        return 1;
    }
    if(k->is("sun_elevation")||k->is("sun_azimuth")||k->is("sun_intensity")
       ||k->is("sky_intensity")||k->is("turbidity")||k->is("ground_bounce")
       ||k->is("sky_mix")||k->is("studio_mix")
       ||k->is("studio_key_azimuth")||k->is("studio_key_elevation")
       ||k->is("studio_key_intensity")||k->is("studio_fill_ratio")
       ||k->is("studio_rim_intensity"))return 1;
    
    if(k->is("quality_preset")){
        // Custom(0), Draft(1), Preview(2), Production(3), Final(4), Ultra(5)
        // Step size = 1/(q*q). Lower = finer detail but slower.
       struct QPreset { double q; int sh; double shDen; int deep; double envD; int rs; };
        static const QPreset qv[]={
            {},                               // 0: Custom
            {1.5,  4, 1.0,  16, 0.3,  1},   // 1: Draft
            {3.0,  8, 1.0,  32, 0.4,  1},   // 2: Preview
            {5.0, 16, 1.0,  48, 0.6,  4},   // 3: Production
            {7.0, 24, 1.0,  64, 0.7,  9},   // 4: Final
            {10.0,32, 1.0, 128, 1.0, 16},   // 5: Ultra
        };
        if(_qualityPreset>0&&_qualityPreset<(int)(sizeof(qv)/sizeof(QPreset))){
            const auto&q=qv[_qualityPreset];
            knob("quality")->set_value(q.q);_quality=q.q;
            knob("shadow_steps")->set_value(q.sh);_shadowSteps=q.sh;
            knob("shadow_density")->set_value(q.shDen);_shadowDensity=q.shDen;
            knob("deep_samples")->set_value(q.deep);_deepSamples=q.deep;
            knob("env_diffuse")->set_value(q.envD);_envDiffuse=q.envD;
            knob("render_samples")->set_value(q.rs);_renderSamples=q.rs;}
        return 1;}
        if(k->is("chromatic_ext")){
        if(_chromaticExt){
            knob("ext_r")->set_value(_extinction); _extR=_extinction;
            knob("ext_g")->set_value(_extinction); _extG=_extinction;
            knob("ext_b")->set_value(_extinction); _extB=_extinction;
        }
        return 1;}
    if(k->is("ext_r")||k->is("ext_g")||k->is("ext_b"))return 1;
    if(k->is("powder_strength")||k->is("gradient_mix")||k->is("jitter"))return 1;
    if(k->is("g_forward")||k->is("g_backward")||k->is("lobe_mix"))return 1;
    if(k->is("ms_approx")||k->is("ms_tint"))return 1;
    if(k->is("shadow_cache")||k->is("shadow_cache_res")){_shadowCacheDirty=true;return 1;}
    if(k->is("phase_mode")||k->is("mie_droplet_d"))return 1;
    if(k->is("render_samples"))return 1;
    if(k->is("noise_enable")||k->is("noise_scale")||k->is("noise_strength")||
       k->is("noise_octaves")||k->is("noise_roughness"))return 1;
    if(k->is("env_mode")||k->is("env_virtual_lights")){_envDirty=true;return 1;}
    if(k->is("use_restir"))return 1;
    return Iop::knob_changed(k);
}

// ═══ Inputs ═══

CameraOp* VDBRenderIop::camera() const{return dynamic_cast<CameraOp*>(Op::input(1));}

const char* VDBRenderIop::input_label(int idx,char*buf) const{
    if(idx==0)return"bg";
    if(idx==1)return"cam";
    if(idx==2)return"scn";
    return nullptr;}

bool VDBRenderIop::test_input(int idx,Op*op) const{
    if(idx==0)return dynamic_cast<Iop*>(op)!=nullptr;
    if(idx==1)return dynamic_cast<CameraOp*>(op)||Iop::test_input(idx,op);
    if(idx==2)return true; // Scene, Axis, Light, EnvironLight — anything 3D
    return Iop::test_input(idx,op);}

Op* VDBRenderIop::default_input(int idx) const{
    if(idx==0)return Iop::default_input(idx);
    return nullptr;}

// ═══ Light Rig — analytical sun/sky + studio setups ═══

static void sunColorFromElevation(double elev, double turbidity, double& r, double& g, double& b) {
    // Simplified Preetham: sun colour shifts from white(noon) → gold → orange → red(horizon)
    // Turbidity increases warmth and reduces blue
    double t = std::clamp(elev / 90.0, 0.0, 1.0); // 0=horizon, 1=zenith
    double turbShift = (turbidity - 2.0) / 8.0 * 0.15; // haze warms the sun
    r = 1.0;
    g = std::clamp(0.4 + 0.55 * t - turbShift, 0.15, 1.0);
    b = std::clamp(0.1 + 0.8 * t * t - turbShift * 2, 0.02, 0.95);
}

static void skyColorFromElevation(double elev, double turbidity, double& r, double& g, double& b) {
    // Sky dome colour: deep blue at noon, warm purple at sunset, grey at high turbidity
    double t = std::clamp(elev / 90.0, 0.0, 1.0);
    double haze = std::clamp((turbidity - 2.0) / 8.0, 0.0, 1.0);
    // Base blue sky, lerp toward warm at low elevation
    r = 0.15 + 0.1 * (1 - t) + 0.3 * haze;
    g = 0.3 + 0.15 * (1 - t) + 0.15 * haze;
    b = 0.7 - 0.3 * (1 - t) - 0.2 * haze;
    // Desaturate with turbidity
    double grey = (r + g + b) / 3.0;
    r = r + (grey - r) * haze * 0.5;
    g = g + (grey - g) * haze * 0.5;
    b = b + (grey - b) * haze * 0.5;
}

static void dirFromElevAzim(double elevDeg, double azimDeg, double& dx, double& dy, double& dz) {
    double er = elevDeg * M_PI / 180.0, ar = azimDeg * M_PI / 180.0;
    dx = std::cos(er) * std::sin(ar);
    dy = std::sin(er);
    dz = -std::cos(er) * std::cos(ar);
    double len = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (len > 1e-8) { dx /= len; dy /= len; dz /= len; }
}

void VDBRenderIop::buildLightRig() {
    // [V3.1] Rig lights always added on top of scene lights.
    // Previously scene lights suppressed the rig entirely — now they compose.
    // Turn Sky Mix and Studio Mix to 0 to disable the rig manually.
    if (_skyMix < 0.001 && _studioMix < 0.001 && !_useFallbackLight) return;

    auto addLight = [&](double dx, double dy, double dz, double r, double g, double b) {
        CachedLight cl;
        cl.dir[0]=dx;cl.dir[1]=dy;cl.dir[2]=dz;
        cl.color[0]=r;cl.color[1]=g;cl.color[2]=b;
        cl.isPoint=false;cl.pos[0]=cl.pos[1]=cl.pos[2]=0;
        _lights.push_back(cl);
    };

    // ── Sun and Sky (driven by sliders, scaled by _skyMix) ──
    if (_skyMix > 0.001) {
        double m = _skyMix;
        double sunR,sunG,sunB,skyR,skyG,skyB,sdx,sdy,sdz;
        sunColorFromElevation(_sunElevation, _turbidity, sunR, sunG, sunB);
        skyColorFromElevation(_sunElevation, _turbidity, skyR, skyG, skyB);
        dirFromElevAzim(_sunElevation, _sunAzimuth, sdx, sdy, sdz);
        // Sun — dim near horizon naturally
        double si = _sunIntensity * std::clamp(_sunElevation / 15.0, 0.1, 1.0) * m;
        if (si > 0.001)
            addLight(sdx, sdy, sdz, sunR*si, sunG*si, sunB*si);
        // Sky dome
        double ski = _skyIntensity * m;
        if (ski > 0.001)
            addLight(0, 1, 0, skyR*ski, skyG*ski, skyB*ski);
        // Ground bounce
        double gb = _groundBounce * m;
        if (gb > 0.01)
            addLight(0, -1, 0, gb*0.8, gb*0.7, gb*0.5);
    }

    // ── Studio lights (driven by sliders, scaled by _studioMix) ──
    if (_studioMix > 0.001) {
        double m = _studioMix;
        double kaz = _studioKeyAzimuth;
        double kel = _studioKeyElevation;
        // Key light — slightly warm
        double ki = _studioKeyIntensity * m;
        if (ki > 0.001) {
            double kx,ky,kz; dirFromElevAzim(kel, kaz, kx, ky, kz);
            addLight(kx, ky, kz, ki, ki*0.95, ki*0.9);
        }
        // Fill light — opposite side, lower, slightly cool
        double fi = ki * _studioFillRatio;
        if (fi > 0.001) {
            double fx,fy,fz; dirFromElevAzim(kel*0.5, kaz+180, fx, fy, fz);
            addLight(fx, fy, fz, fi*0.9, fi*0.92, fi);
        }
        // Rim light — behind and high, independent of key
        double ri2 = _studioRimIntensity * m;
        if (ri2 > 0.001) {
            double rx,ry,rz; dirFromElevAzim(kel+15, kaz+160, rx, ry, rz);
            addLight(rx, ry, rz, ri2, ri2, ri2);
        }
    }

    // ── Custom manual light ──
    if (_useFallbackLight && _lightIntensity > 0.001) {
        double ld = std::sqrt(_lightDir[0]*_lightDir[0]+_lightDir[1]*_lightDir[1]+_lightDir[2]*_lightDir[2]);
        if (ld > 1e-8)
            addLight(_lightDir[0]/ld, _lightDir[1]/ld, _lightDir[2]/ld,
                     _lightColor[0]*_lightIntensity, _lightColor[1]*_lightIntensity, _lightColor[2]*_lightIntensity);
    }
}

// ═══ Scene input — gather lights, axis, environment ═══

// Recursively walk the scene input tree to find LightOps, Environment, and AxisOps
void VDBRenderIop::gatherLights(Op* scnOp) {
    if(!scnOp) return;
    scnOp->validate(true);

    // Check class name first — Environment nodes are NOT LightOp subclasses
    std::string cls(scnOp->Class());

    // Classic Environment node (HDRI on input 1)
    if(cls=="Environment"){
        Iop* envIop=nullptr;
        for(int i=0;i<scnOp->inputs();++i){
            envIop=dynamic_cast<Iop*>(scnOp->input(i));
            if(envIop) break;
        }
        if(envIop&&_envIntensity>0){
            envIop->validate(true);
            if(envIop!=_envIop||!_hasEnvMap){_envIop=envIop;_envDirty=true;}
            if(Knob*rk=scnOp->knob("rotate")){
                _envLightRotY=rk->get_value_at(outputContext().frame(),1);}
        }
        return;
    }

    // New EnvironmentLight node (has file knob, no HDRI input)
    if(cls=="EnvironmentLight"){
        if(Knob*fk=scnOp->knob("file")){
            const char*fp=fk->get_text();
            if(fp&&fp[0]&&_envIntensity>0){
                // Read the HDRI file via a temporary Read node approach
                // For now, store the path — we'll load it in _open
                _envFilePath=std::string(fp);
                _envDirty=true;
                if(Knob*rk=scnOp->knob("rotate")){
                    _envLightRotY=rk->get_value_at(outputContext().frame(),1);}
            }
        }
        return;
    }

    // Check if this op is a regular Light
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
    hash.append(_skyPreset);hash.append(_studioPreset);hash.append(_skyMix);hash.append(_studioMix);
    hash.append(_sunElevation);hash.append(_sunAzimuth);
    hash.append(_sunIntensity);hash.append(_skyIntensity);hash.append(_turbidity);hash.append(_groundBounce);
    hash.append(_studioKeyAzimuth);hash.append(_studioKeyElevation);
    hash.append(_studioKeyIntensity);hash.append(_studioFillRatio);hash.append(_studioRimIntensity);
    hash.append(_motionBlur);hash.append(_shutterOpen);hash.append(_shutterClose);hash.append(_motionSamples);
    hash.append(_aovDensity);hash.append(_aovEmission);hash.append(_aovShadow);hash.append(_aovDepth);
    hash.append(_gForward);hash.append(_gBackward);hash.append(_lobeMix);
    hash.append(_powderStrength);hash.append(_gradientMix);hash.append(_jitter);
    hash.append(_msApprox);
    hash.append(_msTint[0]);hash.append(_msTint[1]);hash.append(_msTint[2]);
    hash.append(_chromaticExt);hash.append(_extR);hash.append(_extG);hash.append(_extB);
    hash.append(_useShadowCache);hash.append(_shadowCacheRes);
    hash.append(_renderSamples);
    hash.append(_noiseEnable);hash.append(_noiseScale);hash.append(_noiseStrength);
    hash.append(_noiseOctaves);hash.append(_noiseRoughness);
    hash.append(_phaseMode);hash.append(_mieDropletD);
    hash.append(_envMode);hash.append(_envVirtualLights);
    hash.append(_useReSTIR);
    for(int i=0;i<3;++i){hash.append(_lightDir[i]);hash.append(_lightColor[i]);}
    if(input(1))hash.append(Op::input(1)->hash());
    if(input(2))hash.append(Op::input(2)->hash());
#ifdef VDBRENDER_HAS_NEURAL
    hash.append(_neuralMode);
    hash.append(_neuralUseCuda);
    hash.append(_neuralDecodeToGrid);
    hash.append(_neuralBatchSize);
#endif
}

// ═══ 3D Viewport ═══

void VDBRenderIop::build_handles(ViewerContext*ctx){
    if(!_gridValid)return;
    // Always add draw handle so Nuke can frame the volume (F key)
    add_draw_handle(ctx);
}

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

    // Always compute transformed corners for F-key framing
    float co[8][3];int ci=0;
    for(int iz=0;iz<=1;++iz)for(int iy=0;iy<=1;++iy)for(int ix=0;ix<=1;++ix){
        double wx=ix?_bboxMax[0]:_bboxMin[0],wy=iy?_bboxMax[1]:_bboxMin[1],wz=iz?_bboxMax[2]:_bboxMin[2];
        if(_hasVolumeXform){double tx=_volFwd[0][0]*wx+_volFwd[1][0]*wy+_volFwd[2][0]*wz+_volFwd[3][0];
            double ty=_volFwd[0][1]*wx+_volFwd[1][1]*wy+_volFwd[2][1]*wz+_volFwd[3][1];
            double tz=_volFwd[0][2]*wx+_volFwd[1][2]*wy+_volFwd[2][2]*wz+_volFwd[3][2];
            wx=tx;wy=ty;wz=tz;}
        co[ci][0]=(float)wx;co[ci][1]=(float)wy;co[ci][2]=(float)wz;++ci;}

    if(_showBbox){
        glLineWidth(1.5f);glColor3f(0,1,0);glBegin(GL_LINES);
        for(int e=0;e<12;++e){glVertex3fv(co[_bboxEdges[e][0]]);glVertex3fv(co[_bboxEdges[e][1]]);}glEnd();
        float cx=0,cy=0,cz=0;for(int i=0;i<8;++i){cx+=co[i][0];cy+=co[i][1];cz+=co[i][2];}
        cx/=8;cy/=8;cz/=8;float sz=0;
        for(int i=0;i<8;++i){float d=std::sqrt((co[i][0]-cx)*(co[i][0]-cx)+(co[i][1]-cy)*(co[i][1]-cy)+(co[i][2]-cz)*(co[i][2]-cz));if(d>sz)sz=d;}
        sz*=.05f;glColor3f(1,1,0);glBegin(GL_LINES);
        glVertex3f(cx-sz,cy,cz);glVertex3f(cx+sz,cy,cz);glVertex3f(cx,cy-sz,cz);glVertex3f(cx,cy+sz,cz);
        glVertex3f(cx,cy,cz-sz);glVertex3f(cx,cy,cz+sz);glEnd();
    } else {
        // Draw bbox edges with near-zero alpha so Nuke can frame to them (F key)
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(0.5f);glColor4f(0.2f,0.2f,0.2f,0.01f);glBegin(GL_LINES);
        for(int e=0;e<12;++e){glVertex3fv(co[_bboxEdges[e][0]]);glVertex3fv(co[_bboxEdges[e][1]]);}glEnd();
    }
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

    // ── Custom Fill Light handle ─────────────────────────────────────────────
    // Drawn when the light is enabled. Shows:
    //   · A dashed line from the volume centre toward the light source
    //     (opposite the direction vector — where the light comes FROM)
    //   · An arrowhead at the outer end indicating incoming light direction
    //   · A small crosshair disc at the light source position
    //   · A short solid line from centre toward the volume, showing
    //     how the light enters the volume
    if (_useFallbackLight) {
        // Compute volume world-space centre
        float cx=0,cy=0,cz=0;
        for(int i=0;i<8;++i){cx+=co[i][0];cy+=co[i][1];cz+=co[i][2];}
        cx/=8; cy/=8; cz/=8;

        // Bbox diagonal — used to scale handle length sensibly
        float bx=_bboxMax[0]-_bboxMin[0], by=_bboxMax[1]-_bboxMin[1], bz=_bboxMax[2]-_bboxMin[2];
        float bdiag = std::sqrt(bx*bx + by*by + bz*bz);
        float armLen = bdiag * 0.7f;   // dashed arm to light source
        float arrowLen = bdiag * 0.08f; // arrowhead size

        // Normalise light direction (travels FROM source TO volume)
        float lx=(float)_lightDir[0], ly=(float)_lightDir[1], lz=(float)_lightDir[2];
        float llen = std::sqrt(lx*lx + ly*ly + lz*lz);
        if (llen < 1e-6f) { llen = 1.0f; }
        lx/=llen; ly/=llen; lz/=llen;

        // _lightDir points TOWARD the light source (same as shadow ray direction).
        // So the source disc sits at centre + direction * armLen.
        float sx = cx + lx * armLen;
        float sy = cy + ly * armLen;
        float sz = cz + lz * armLen;

        // Light colour tinted by the knob colour × intensity (clamped to [0.3,1])
        float lr = std::clamp((float)(_lightColor[0] * _lightIntensity), 0.3f, 1.0f);
        float lg2= std::clamp((float)(_lightColor[1] * _lightIntensity), 0.3f, 1.0f);
        float lb2= std::clamp((float)(_lightColor[2] * _lightIntensity), 0.3f, 1.0f);

        glEnable(GL_LINE_STIPPLE);
        glLineStipple(3, 0xAAAA);  // dashed: ----  ----
        glLineWidth(1.5f);
        glColor3f(lr, lg2, lb2);

        // Dashed arm: light source → volume centre
        glBegin(GL_LINES);
        glVertex3f(sx, sy, sz);
        glVertex3f(cx, cy, cz);
        glEnd();

        glDisable(GL_LINE_STIPPLE);

        // Solid short inner arrow: centre → toward source (shows incoming direction)
        glLineWidth(2.0f);
        glBegin(GL_LINES);
        glVertex3f(cx, cy, cz);
        glVertex3f(cx + lx*arrowLen*0.5f, cy + ly*arrowLen*0.5f, cz + lz*arrowLen*0.5f);
        glEnd();

        // Arrowhead at the light source end (3 lines forming a cone tip)
        // Build two perpendicular vectors to lx/ly/lz for the arrowhead fins
        float ax=0, ay=1, az=0;
        if (std::abs(ly) > 0.9f) { ax=1; ay=0; az=0; }
        // Gram-Schmidt: perp1 = ax - (ax·l)*l
        float dot1 = ax*lx + ay*ly + az*lz;
        float p1x=ax-dot1*lx, p1y=ay-dot1*ly, p1z=az-dot1*lz;
        float p1len=std::sqrt(p1x*p1x+p1y*p1y+p1z*p1z);
        if(p1len>1e-6f){p1x/=p1len;p1y/=p1len;p1z/=p1len;}
        // perp2 = l × perp1
        float p2x=ly*p1z-lz*p1y, p2y=lz*p1x-lx*p1z, p2z=lx*p1y-ly*p1x;

        // Arrow tip = light source pos, fins point toward the volume (negate dir)
        float tipX=sx, tipY=sy, tipZ=sz;
        float finLen=arrowLen, finSpread=arrowLen*0.35f;
        glLineWidth(1.5f);
        glBegin(GL_LINES);
        // 4 fins pointing from source toward volume (-direction)
        glVertex3f(tipX,tipY,tipZ);
        glVertex3f(tipX-lx*finLen+p1x*finSpread, tipY-ly*finLen+p1y*finSpread, tipZ-lz*finLen+p1z*finSpread);
        glVertex3f(tipX,tipY,tipZ);
        glVertex3f(tipX-lx*finLen-p1x*finSpread, tipY-ly*finLen-p1y*finSpread, tipZ-lz*finLen-p1z*finSpread);
        glVertex3f(tipX,tipY,tipZ);
        glVertex3f(tipX-lx*finLen+p2x*finSpread, tipY-ly*finLen+p2y*finSpread, tipZ-lz*finLen+p2z*finSpread);
        glVertex3f(tipX,tipY,tipZ);
        glVertex3f(tipX-lx*finLen-p2x*finSpread, tipY-ly*finLen-p2y*finSpread, tipZ-lz*finLen-p2z*finSpread);
        glEnd();

        // Crosshair disc at light source (8 short spokes)
        float discR = arrowLen * 0.5f;
        glPointSize(5.0f);
        glBegin(GL_POINTS);
        glVertex3f(sx, sy, sz);
        glEnd();
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        for (int s=0; s<8; ++s) {
            float ang = (float)s * (float)M_PI / 4.0f;
            float ux = std::cos(ang)*p1x + std::sin(ang)*p2x;
            float uy = std::cos(ang)*p1y + std::sin(ang)*p2y;
            float uz = std::cos(ang)*p1z + std::sin(ang)*p2z;
            glVertex3f(sx+ux*discR*0.3f, sy+uy*discR*0.3f, sz+uz*discR*0.3f);
            glVertex3f(sx+ux*discR,      sy+uy*discR,      sz+uz*discR);
        }
        glEnd();
    }

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

    // Scene input (2): gather lights + axis + EnvironLight from scene tree
    for(int i=0;i<4;++i)for(int j=0;j<4;++j)_volFwd[i][j]=_volInv[i][j]=(i==j)?1:0;
    _envLightRotY=0;_envDirty=false;
    {Iop*prevEnv=_envIop;_envIop=nullptr;
    if(input(2)) gatherLights(Op::input(2));
    // If no EnvironLight found, clear env map
    if(!_envIop){_hasEnvMap=false;_hasEnvSH=false;_envDirty=false;}
    }

    // Light rig — generates lights when none found in scene
    buildLightRig();
    _shadowCacheDirty = true;   // [V3] lights changed — rebuild cache next _open()

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

    // Load VDB or NVDB
    {Guard guard(_loadLock);
    int curFrame=(int)outputContext().frame()+_frameOffset;
    std::string path2=resolveFramePath(curFrame);std::string grid(_gridName?_gridName:"");
    {std::string tp=path2;for(auto&c:tp)if(c=='\\')c='/';FILE*f=fopen(tp.c_str(),"rb");
     if(!f){std::string orig(_vdbFilePath?_vdbFilePath:"");if(orig!=path2){for(auto&c:orig)if(c=='\\')c='/';
         FILE*of=fopen(orig.c_str(),"rb");if(of){fclose(of);path2=orig;}}}else fclose(f);}

#ifdef VDBRENDER_HAS_NEURAL
    // ── Neural VDB path (.nvdb files) ──
    bool isNVDB=false;
    {size_t len=path2.size();
     if(len>5&&(path2.substr(len-5)==".nvdb"||path2.substr(len-5)==".NVDB"))isNVDB=true;}

    if(isNVDB){
        if(!_gridValid||path2!=_loadedPath||curFrame!=_loadedFrame){
            if(!_neural)_neural=std::make_unique<neural::NeuralDecoder>();
            _neural->unload();_neuralMode=false;
            _floatGrid.reset();_tempGrid.reset();_flameGrid.reset();_velGrid.reset();_colorGrid.reset();
            _gridValid=false;_hasTempGrid=false;_hasFlameGrid=false;_hasVelGrid=false;_hasColorGrid=false;_previewPoints.clear();

            std::string cp2=path2;for(auto&c:cp2)if(c=='\\')c='/';
            if(_neural->load(cp2,_neuralUseCuda)){

                if(_neuralDecodeToGrid){
                    // DECODE-TO-GRID: batched neural decode → standard FloatGrid
                    // Rendering uses BoxSampler at full speed, zero neural overhead
                    auto decoded=_neural->decodeToGrid(_neuralBatchSize);
                    if(decoded&&decoded->activeVoxelCount()>0){
                        _floatGrid=decoded;
                        _neuralMode=false; // BoxSampler path — no per-sample neural dispatch
                    }else{
                        // Fallback to upper tree if decode fails
                        _floatGrid=_neural->upperGrid();
                        _neuralMode=true;
                    }
                }else{
                    // LIVE NEURAL: per-sample inference during rendering (lower memory)
                    _floatGrid=_neural->upperGrid();
                    _neuralMode=true;
                }

                auto ab=_floatGrid->evalActiveVoxelBoundingBox();
                if(!ab.empty()){
                    const auto&xf=_floatGrid->transform();
                    openvdb::Vec3d corners[8];int ci2=0;
                    for(int iz=0;iz<=1;++iz)for(int iy=0;iy<=1;++iy)for(int ix=0;ix<=1;++ix)
                        corners[ci2++]=xf.indexToWorld(openvdb::Vec3d(ix?ab.max().x()+1.:ab.min().x(),
                            iy?ab.max().y()+1.:ab.min().y(),iz?ab.max().z()+1.:ab.min().z()));
                    _bboxMin=_bboxMax=corners[0];
                    for(int i=1;i<8;++i)for(int a=0;a<3;++a){
                        _bboxMin[a]=std::min(_bboxMin[a],corners[i][a]);
                        _bboxMax[a]=std::max(_bboxMax[a],corners[i][a]);}
                    _gridValid=true;_loadedPath=path2;_loadedGrid=grid;_loadedFrame=curFrame;
                    _volRI.reset();
                    try{_volRI=std::make_unique<VRI>(*_floatGrid);}catch(...){}

                    char buf[64];
                    std::snprintf(buf,sizeof(buf),"%.1fx",_neural->ratio());
                    _neuralInfoRatioStr=buf;_neuralInfoRatio=_neuralInfoRatioStr.c_str();
                    std::snprintf(buf,sizeof(buf),"%.1f dB",_neural->psnr());
                    _neuralInfoPSNRStr=buf;_neuralInfoPSNR=_neuralInfoPSNRStr.c_str();

                    // Mode info
                    _neuralInfoModeStr=_neuralMode?"Live Neural":"Decoded to Grid";
                    _neuralInfoMode=_neuralInfoModeStr.c_str();
                }else{error("NeuralVDB: no active voxels.");}
            }else{error("Failed to load .nvdb file: %s",path2.c_str());}
        }
    }else{
        _neuralMode=false;
#endif

    // ── Standard OpenVDB path (.vdb files) ──
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
    }

#ifdef VDBRENDER_HAS_NEURAL
    } // close else from isNVDB check
#endif
    }
}

// ═══ [V3] Transmittance cache ═════════════════════════════════════════════════
// Sweeps the voxel grid once per directional light using a dominant-axis
// slice-by-slice pass, storing T(voxel) = transmittance from that voxel
// toward the light. Called from _open() on the main thread before rendering.

void VDBRenderIop::buildShadowCaches() {
    _shadowCaches.clear();
    if (!_floatGrid || !_useShadowCache) return;

    const auto& xf   = _floatGrid->transform();
    const double ext  = _extinction * _shadowDensity;
    // Step size: voxel world size × cache resolution multiplier
    const int    resMul  = 1 << std::clamp(_shadowCacheRes, 0, 2); // 1, 2 or 4
    const double voxSize = xf.voxelSize()[0] * resMul;

    const auto ab = _floatGrid->evalActiveVoxelBoundingBox();
    if (ab.empty()) return;

    // Expand by a small margin so boundary voxels are fully covered
    const openvdb::Coord lo = ab.min() - openvdb::Coord(2);
    const openvdb::Coord hi = ab.max() + openvdb::Coord(2);

    for (const auto& lt : _lights) {
        ShadowCache sc;
        sc.lightDir = openvdb::Vec3d(lt.dir[0], lt.dir[1], lt.dir[2]);

        if (lt.isPoint) {
            // Point lights use HDDA per-sample — no cache
            sc.valid = false;
            _shadowCaches.push_back(std::move(sc));
            continue;
        }

        // Transmittance grid with same topology as density grid
        sc.transGrid = openvdb::FloatGrid::create(1.0f);
        sc.transGrid->setTransform(xf.copy());
        auto tAcc = sc.transGrid->getAccessor();
        auto dAcc = _floatGrid->getConstAccessor();

        const openvdb::Vec3d& lD = sc.lightDir;

        // Dominant axis of the light direction
        int domAxis = 0;
        double mx = std::abs(lD[0]);
        for (int a = 1; a < 3; ++a)
            if (std::abs(lD[a]) > mx) { mx = std::abs(lD[a]); domAxis = a; }
        const int ax1 = (domAxis + 1) % 3;
        const int ax2 = (domAxis + 2) % 3;

        // Coordinate accessor helpers
        auto getCoordDom = [&](const openvdb::Coord& c) { return c[domAxis]; };
        auto getCoordAx1 = [&](const openvdb::Coord& c) { return c[ax1]; };
        auto getCoordAx2 = [&](const openvdb::Coord& c) { return c[ax2]; };

        const int iMin = getCoordDom(lo);
        const int iMax = getCoordDom(hi);
        const int j0   = getCoordAx1(lo), j1 = getCoordAx1(hi);
        const int k0   = getCoordAx2(lo), k1 = getCoordAx2(hi);

        // lightward: light arrives from +domAxis side
        const bool lightward = lD[domAxis] < 0.0;

        auto makeCoord = [&](int i, int j, int k) {
            openvdb::Coord c;
            c[domAxis] = i; c[ax1] = j; c[ax2] = k;
            return c;
        };

        for (int j = j0; j <= j1; j += resMul) {
            for (int k = k0; k <= k1; k += resMul) {
                float prevT = 1.0f;

                auto processVoxel = [&](int i) {
                    const openvdb::Coord coord = makeCoord(i, j, k);
                    const float density = dAcc.getValue(coord);
                    // Store transmittance from this voxel toward the light
                    // (= transmittance before this voxel's own absorption)
                    if (resMul > 1) {
                        // Write to a block of voxels at full resolution
                        for (int di = 0; di < resMul; ++di)
                        for (int dj = 0; dj < resMul; ++dj)
                        for (int dk = 0; dk < resMul; ++dk) {
                            openvdb::Coord fc;
                            fc[domAxis] = i + di; fc[ax1] = j + dj; fc[ax2] = k + dk;
                            tAcc.setValue(fc, prevT);
                        }
                    } else {
                        tAcc.setValue(coord, prevT);
                    }
                    prevT *= std::exp(-(float)(density * ext * voxSize));
                };

                if (lightward) {
                    for (int i = iMax; i >= iMin; i -= resMul) processVoxel(i);
                } else {
                    for (int i = iMin; i <= iMax; i += resMul) processVoxel(i);
                }
            }
        }

        sc.valid = true;
        _shadowCaches.push_back(std::move(sc));
    }

    // ── [V6] Env dir shadow caches ──────────────────────────────────────────
    // Build transmittance caches for the 6 axis directions used by the SH env
    // path. These replace HDDA shadow rays in the 6-dir SH loop with O(1)
    // lookups — completing the O(1) env lighting chain at full quality.
    // Only built when an env map is loaded (SH mode only uses these).
    _envDirCacheBase = -1;
    if (_hasEnvSH) {
        _envDirCacheBase = (int)_shadowCaches.size();

        const openvdb::Vec3d kAxisDirs[6] = {
            {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

        for (int di = 0; di < 6; ++di) {
            ShadowCache sc;
            sc.lightDir = kAxisDirs[di];
            sc.transGrid = openvdb::FloatGrid::create(1.0f);
            sc.transGrid->setTransform(xf.copy());
            auto tAcc = sc.transGrid->getAccessor();
            auto dAcc = _floatGrid->getConstAccessor();

            const openvdb::Vec3d& lD = sc.lightDir;

            int domAxis = 0;
            double mx = std::abs(lD[0]);
            for (int a = 1; a < 3; ++a)
                if (std::abs(lD[a]) > mx) { mx = std::abs(lD[a]); domAxis = a; }
            const int ax1 = (domAxis + 1) % 3;
            const int ax2 = (domAxis + 2) % 3;

            auto getD = [&](const openvdb::Coord& c, int axis) { return c[axis]; };
            const int iMin = getD(lo, domAxis), iMax = getD(hi, domAxis);
            const int j0   = getD(lo, ax1),     j1   = getD(hi, ax1);
            const int k0   = getD(lo, ax2),     k1   = getD(hi, ax2);
            const bool lightward = lD[domAxis] < 0.0;

            auto makeCoord = [&](int i, int j, int k) {
                openvdb::Coord c; c[domAxis]=i; c[ax1]=j; c[ax2]=k; return c; };

            for (int j = j0; j <= j1; j += resMul) {
                for (int k = k0; k <= k1; k += resMul) {
                    float prevT = 1.0f;
                    auto processVoxel = [&](int i) {
                        const openvdb::Coord coord = makeCoord(i, j, k);
                        const float density = dAcc.getValue(coord);
                        if (resMul > 1) {
                            for (int di2=0;di2<resMul;++di2)
                            for (int dj=0;dj<resMul;++dj)
                            for (int dk=0;dk<resMul;++dk) {
                                openvdb::Coord fc;
                                fc[domAxis]=i+di2; fc[ax1]=j+dj; fc[ax2]=k+dk;
                                tAcc.setValue(fc, prevT);
                            }
                        } else { tAcc.setValue(coord, prevT); }
                        prevT *= std::exp(-(float)(density * ext * voxSize));
                    };
                    if (lightward) for (int i=iMax;i>=iMin;i-=resMul) processVoxel(i);
                    else           for (int i=iMin;i<=iMax;i+=resMul) processVoxel(i);
                }
            }
            sc.valid = true;
            _shadowCaches.push_back(std::move(sc));
        }
    }
}

// ═══ engine ═══

void VDBRenderIop::_request(int x,int y,int r,int t,ChannelMask channels,int count){
    Iop*bg=dynamic_cast<Iop*>(Op::input(0));
    if(bg) bg->request(x,y,r,t,Mask_RGBA,count);  // only RGBA from BG, not AOV channels
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
        // Virtual lights were just appended to _lights — force shadow cache rebuild
        _shadowCacheDirty=true;
    }
    // [V3] Build transmittance caches for directional lights (including virtual env lights)
    if (_useShadowCache && _shadowCacheDirty) {
        buildShadowCaches();
        _shadowCacheDirty = false;
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
     if(bg){bg->get(y,x,r,Mask_RGBA,bgRow);hasBg=true;}}

    if(!_gridValid||!_camValid||(!_floatGrid&&!_tempGrid&&!_flameGrid&&!_hasColorGrid)){
        foreach(z,channels){float*p=row.writable(z);
            if(hasBg&&(z==Chan_Red||z==Chan_Green||z==Chan_Blue||z==Chan_Alpha)){
                const float*bp=bgRow[z];
                for(int i=x;i<r;++i)p[i]=bp[i];}
            else{for(int i=x;i<r;++i)p[i]=0;}}
        return;}
    const Format&fmt=format();int W=fmt.width(),H=fmt.height();
    // Proxy: effective render resolution
    int pW=(int)(W*pxScale),pH=(int)(H*pxScale);if(pW<1)pW=1;if(pH<1)pH=1;
    double halfW=_halfW,halfH=_halfW*(double)H/(double)W;
    float*rO=row.writable(Chan_Red),*gO=row.writable(Chan_Green),*bO=row.writable(Chan_Blue),*aO=row.writable(Chan_Alpha);
    ColorScheme scheme=static_cast<ColorScheme>(_colorScheme);
    float gA[3]={(float)_gradStart[0],(float)_gradStart[1],(float)_gradStart[2]};

    // Create march context once per scanline (avoids per-pixel accessor creation)
    MarchCtx ctx=makeMarchCtx();

    // Pre-compute camera origin transform (same for all pixels in this row)
    double ox=_camOrigin[0],oy=_camOrigin[1],oz=_camOrigin[2];
    if(_hasVolumeXform){
        double tx=_volInv[0][0]*ox+_volInv[1][0]*oy+_volInv[2][0]*oz+_volInv[3][0];
        double ty=_volInv[0][1]*ox+_volInv[1][1]*oy+_volInv[2][1]*oz+_volInv[3][1];
        double tz=_volInv[0][2]*ox+_volInv[1][2]*oy+_volInv[2][2]*oz+_volInv[3][2];
        ox=tx;oy=ty;oz=tz;
    }
    float gB[3]={(float)_gradEnd[0],(float)_gradEnd[1],(float)_gradEnd[2]};

    // AOV channel pointers — only get writable if channel is in the requested set
    Channel chDenR=_aovDensity?channel("vdb_density.red"):Chan_Black;
    Channel chDenG=_aovDensity?channel("vdb_density.green"):Chan_Black;
    Channel chDenB=_aovDensity?channel("vdb_density.blue"):Chan_Black;
    Channel chEmR=_aovEmission?channel("vdb_emission.red"):Chan_Black;
    Channel chEmG=_aovEmission?channel("vdb_emission.green"):Chan_Black;
    Channel chEmB=_aovEmission?channel("vdb_emission.blue"):Chan_Black;
    Channel chShR=_aovShadow?channel("vdb_shadow.red"):Chan_Black;
    Channel chShG=_aovShadow?channel("vdb_shadow.green"):Chan_Black;
    Channel chShB=_aovShadow?channel("vdb_shadow.blue"):Chan_Black;
    Channel chDpR=_aovDepth?channel("vdb_depth.red"):Chan_Black;

    float*aovDenR=(_aovDensity&&channels.contains(chDenR))?row.writable(chDenR):nullptr;
    float*aovDenG=(_aovDensity&&channels.contains(chDenG))?row.writable(chDenG):nullptr;
    float*aovDenB=(_aovDensity&&channels.contains(chDenB))?row.writable(chDenB):nullptr;
    float*aovEmR=(_aovEmission&&channels.contains(chEmR))?row.writable(chEmR):nullptr;
    float*aovEmG=(_aovEmission&&channels.contains(chEmG))?row.writable(chEmG):nullptr;
    float*aovEmB=(_aovEmission&&channels.contains(chEmB))?row.writable(chEmB):nullptr;
    float*aovShR=(_aovShadow&&channels.contains(chShR))?row.writable(chShR):nullptr;
    float*aovShG=(_aovShadow&&channels.contains(chShG))?row.writable(chShG):nullptr;
    float*aovShB=(_aovShadow&&channels.contains(chShB))?row.writable(chShB):nullptr;
    float*aovDpP=(_aovDepth&&channels.contains(chDpR))?row.writable(chDpR):nullptr;

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
        if(_hasVolumeXform){
            double dx2=_volInv[0][0]*rdx+_volInv[1][0]*rdy+_volInv[2][0]*rdz;
            double dy2=_volInv[0][1]*rdx+_volInv[1][1]*rdy+_volInv[2][1]*rdz;
            double dz2=_volInv[0][2]*rdx+_volInv[1][2]*rdy+_volInv[2][2]*rdz;
            len=std::sqrt(dx2*dx2+dy2*dy2+dz2*dz2);if(len>1e-8){dx2/=len;dy2/=len;dz2/=len;}rdx=dx2;rdy=dy2;rdz=dz2;}
        openvdb::Vec3d rayO(ox,oy,oz),rayD(rdx,rdy,rdz);
        float ri=(float)_rampIntensity;

        // AABB ray-box test — skip pixels that miss the volume entirely
        {double tE=0,tX=1e30;
         for(int a=0;a<3;++a){
             double inv=(std::abs(rayD[a])>1e-8)?1.0/rayD[a]:1e30;
             double t0=(_bboxMin[a]-rayO[a])*inv,t1=(_bboxMax[a]-rayO[a])*inv;
             if(t0>t1){double tmp=t0;t0=t1;t1=tmp;}
             if(t0>tE)tE=t0;if(t1<tX)tX=t1;}
         if(tE>=tX||tX<=0){
             // Ray misses volume — write BG or black
             if(hasBg){rO[ix]=bgR[ix];gO[ix]=bgG[ix];bO[ix]=bgB[ix];aO[ix]=bgA[ix];}
             else{rO[ix]=0;gO[ix]=0;bO[ix]=0;aO[ix]=0;}
             if(aovDenR){aovDenR[ix]=0;aovDenG[ix]=0;aovDenB[ix]=0;}
             if(aovEmR){aovEmR[ix]=0;aovEmG[ix]=0;aovEmB[ix]=0;}
             if(aovShR){aovShR[ix]=0;aovShG[ix]=0;aovShB[ix]=0;}
             if(aovDpP)aovDpP[ix]=0;
             continue;
         }
        }

        // Accumulate across motion blur time samples AND render samples
        float R=0,G=0,B=0,A=0;
        float emAccR=0,emAccG=0,emAccB=0;

        const int nRenderSamples = std::max(1, _renderSamples);

        for(int si=0;si<nRenderSamples;++si){
        // Per-sample jitter seed — XOR pixel coords with a per-sample constant
        // so each sample explores a different step-offset stratum.
        // Uses the same PCG hash as single-pass jitter, just with different inputs.
        if(ctx.jitter){
            // Combine sample index into hash so all N seeds are uncorrelated
            const uint32_t sx = static_cast<uint32_t>(ix) ^ (static_cast<uint32_t>(si) * 2246822519u);
            const uint32_t sy = static_cast<uint32_t>(y)  ^ (static_cast<uint32_t>(si) * 2654435761u);
            uint32_t u = sx * 2654435761u ^ sy * 2246822519u;
            u ^= u >> 16; u *= 0x45d9f3bu; u ^= u >> 16;
            ctx.jitterOff = (u & 0xFFFFu) / 65536.0;
        }

        for(int ts=0;ts<nTimeSamples;++ts){
            // Motion blur: offset ray origin by velocity * time
            openvdb::Vec3d mO=rayO;
            if(nTimeSamples>1&&ctx.velAcc){
                double tNorm=(nTimeSamples>1)?(double)ts/(nTimeSamples-1):0.5;
                double tSample=_shutterOpen+tNorm*shutterSpan;
                auto iP=_velGrid->transform().worldToIndex(rayO);
                openvdb::Vec3s vel=openvdb::tools::BoxSampler::sample(*ctx.velAcc,iP);
                mO=rayO+openvdb::Vec3d(vel[0],vel[1],vel[2])*tSample;
            }

            float sR=0,sG=0,sB=0,sA=0,sEmR=0,sEmG=0,sEmB=0;
            if(scheme==kLit){marchRay(ctx,mO,rayD,sR,sG,sB,sA,sEmR,sEmG,sEmB);sR*=ri;sG*=ri;sB*=ri;sEmR*=ri;sEmG*=ri;sEmB*=ri;}
            else if(scheme==kExplosion){marchRayExplosion(ctx,mO,rayD,sR,sG,sB,sA,sEmR,sEmG,sEmB);sR*=ri;sG*=ri;sB*=ri;sEmR*=ri;sEmG*=ri;sEmB*=ri;}
            else{float den=0,alpha=0;marchRayDensity(ctx,mO,rayD,den,alpha);
                // Vec3 colour grid override
                if(_hasColorGrid&&ctx.colorAcc&&alpha>1e-6f){
                    auto iPC=_colorGrid->transform().worldToIndex(mO);
                    openvdb::Vec3s cv=openvdb::tools::BoxSampler::sample(*ctx.colorAcc,iPC);
                    sR=cv[0]*ri*alpha;sG=cv[1]*ri*alpha;sB=cv[2]*ri*alpha;sA=alpha;
                }else{
                    Color3 c=evalRamp(scheme,den,gA,gB,_tempMin,_tempMax);
                    sR=c.r*ri*alpha;sG=c.g*ri*alpha;sB=c.b*ri*alpha;sA=alpha;
                }
            }
            R+=sR;G+=sG;B+=sB;A+=sA;
            emAccR+=sEmR;emAccG+=sEmG;emAccB+=sEmB;
        }
        } // end render samples loop

        // Average over motion blur × render samples
        const int totalSamples = nTimeSamples * nRenderSamples;
        if(totalSamples>1){float inv=1.0f/totalSamples;R*=inv;G*=inv;B*=inv;A*=inv;emAccR*=inv;emAccG*=inv;emAccB*=inv;}

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
        if(aovEmR){aovEmR[ix]=emAccR;aovEmG[ix]=emAccG;aovEmB[ix]=emAccB;}
        if(aovShR){float sh=(A>1e-6f)?(1.0f-A):0.0f;aovShR[ix]=sh;aovShG[ix]=sh;aovShB[ix]=sh;}
        if(aovDpP){
            // Compute depth: AABB ray intersection for first-hit distance
            float depth=0;
            if(A>1e-6f){
                double tMin=0,tMax=1e20;
                for(int a=0;a<3;++a){
                    double invD=1.0/((a==0)?rayD[0]:(a==1)?rayD[1]:rayD[2]);
                    double o=(a==0)?rayO[0]:(a==1)?rayO[1]:rayO[2];
                    double bMin=(a==0)?_bboxMin[0]:(a==1)?_bboxMin[1]:_bboxMin[2];
                    double bMax=(a==0)?_bboxMax[0]:(a==1)?_bboxMax[1]:_bboxMax[2];
                    double t1=(bMin-o)*invD, t2=(bMax-o)*invD;
                    if(t1>t2)std::swap(t1,t2);
                    tMin=std::max(tMin,t1);tMax=std::min(tMax,t2);
                }
                if(tMax>=tMin&&tMax>0) depth=(float)std::max(tMin,0.0);
            }
            aovDpP[ix]=depth;
        }
    }
}

// ═══ Environment Map ═══

void VDBRenderIop::cacheEnvMap(Iop* envIop) {
    _hasEnvMap=false;
    _hasEnvSH=false;
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

    // ── [V5] Project env map to L2 spherical harmonics ─────────────────────
    // Ramamoorthi & Hanrahan (2001): MC integration over sphere using
    // equirectangular map. Solid-angle weighting: dΩ = cos(lat) × dlon × dlat.
    std::memset(_envSH, 0, sizeof(_envSH));
    const int shW = kEnvRes, shH = kEnvRes / 2;

    for (int cy = 0; cy < shH; ++cy) {
        // Latitude: v in [0,1] → θ in [-π/2, π/2]
        const double v     = (cy + 0.5) / shH;
        const double theta = (v - 0.5) * M_PI;
        const double cosT  = std::cos(theta);
        const double sinT  = std::sin(theta);
        const double dOmega = cosT * (M_PI / shH) * (2.0 * M_PI / shW);  // dφ × sinθ dθ (correct area element)

        for (int cx = 0; cx < shW; ++cx) {
            const double u   = (cx + 0.5) / shW;
            // Apply env rotation
            double uRot = u + (_envLightRotY + _envRotate) / 360.0;
            uRot -= std::floor(uRot);
            const int sx = std::clamp((int)(uRot * shW), 0, shW-1);

            const float* texel = _envMap[sx][cy];
            const double r = texel[0], g = texel[1], b = texel[2];

            // Direction from equirectangular UV
            // dy negated: Nuke row 0 = bottom of image = south pole in world (+Y up convention)
            // Without negation: cy=0 → dy=-1 but _envMap[cx][0] = Nuke row 0 = bottom = ground
            // evalEnvSH({0,1,0}) queries dy=+1 → should get sky → needs sky at shH-1 → dy=+1 there
            // But sampleEnv with dir[1]=+1 reads cy=top=_envMap top=Nuke top=sky ✓
            // SH projection without negation: cy=0(ground)→dy=-1, cy=top(sky)→dy=+1 ✓
            // The flip must be in the UV→direction phi mapping - negate dy to test
            const double phi = (u - 0.5) * 2.0 * M_PI;   // [-π, π]
            const double dx  =  cosT * std::sin(phi);
            const double dy  = -sinT;   // negated: Nuke bottom-up vs equirect top-down convention
            const double dz  = -cosT * std::cos(phi);

            // L2 SH basis functions
            const double Y[9] = {
                0.282095,
                0.488603 * dy,
                0.488603 * dz,
                0.488603 * dx,
                1.092548 * dx * dy,
                1.092548 * dy * dz,
                0.315392 * (3.0*dz*dz - 1.0),
                1.092548 * dx * dz,
                0.546274 * (dx*dx - dy*dy)
            };

            for (int i = 0; i < 9; ++i) {
                _envSH[i][0] += r * Y[i] * dOmega;
                _envSH[i][1] += g * Y[i] * dOmega;
                _envSH[i][2] += b * Y[i] * dOmega;
            }
        }
    }

    // No extra normalisation needed — dOmega already encodes the correct solid angle.
    // The SH coefficients now directly represent the projection: SH[i] = ∫ L(ω) Y_i(ω) dω

    _hasEnvSH = true;

    // ── [V5] Extract virtual directional lights from brightest env peaks ────
    // Scan env map luminance, extract top N peaks, subtract their contribution
    // from the SH to avoid double-counting, add them as directional CachedLights.
    // They then participate in the V3 transmittance cache automatically.
    const int nVirtual = std::clamp(_envVirtualLights, 0, 4);
    _envVirtualLightBase = (int)_lights.size();

    if (nVirtual > 0) {
        // Build luminance map
        std::vector<std::pair<float,int>> lum(shW * shH);
        for (int cy = 0; cy < shH; ++cy)
        for (int cx = 0; cx < shW; ++cx) {
            const float* t = _envMap[cx][cy];
            lum[cy*shW+cx] = { 0.2126f*t[0] + 0.7152f*t[1] + 0.0722f*t[2],
                                cy*shW+cx };
        }

        for (int vi = 0; vi < nVirtual; ++vi) {
            // Find current maximum
            auto it = std::max_element(lum.begin(), lum.end(),
                [](const auto& a, const auto& b){ return a.first < b.first; });
            if (it->first < 0.1f) break;   // peak too dim to be worth extracting

            const int idx = it->second;
            const int pcx = idx % shW, pcy = idx / shW;

            // Suppress a patch around the peak (avoid extracting the same region twice)
            const int patchR = std::max(1, shW / 16);
            for (int dy = -patchR; dy <= patchR; ++dy)
            for (int dx = -patchR; dx <= patchR; ++dx) {
                int nx = (pcx + dx + shW) % shW;
                int ny = std::clamp(pcy + dy, 0, shH-1);
                lum[ny*shW+nx].first = 0.0f;
            }

            // Convert peak UV to world direction
            const double v2    = (pcy + 0.5) / shH;
            const double theta2 = (v2 - 0.5) * M_PI;
            const double cosT2 = std::cos(theta2), sinT2 = std::sin(theta2);
            double u2 = (pcx + 0.5) / shW;
            u2 += (_envLightRotY + _envRotate) / 360.0;
            u2 -= std::floor(u2);
            const double phi2 = (u2 - 0.5) * 2.0 * M_PI;
            const double vdx  =  cosT2 * std::sin(phi2);
            const double vdy  =  sinT2;
            const double vdz  = -cosT2 * std::cos(phi2);

            // Integrate the patch luminance as the virtual light's colour
            // and subtract it from the SH (rough subtraction: treat as point)
            float vr = 0, vg = 0, vb = 0; int pCount = 0;
            for (int dy = -patchR; dy <= patchR; ++dy)
            for (int dx = -patchR; dx <= patchR; ++dx) {
                int nx = (pcx + dx + shW) % shW;
                int ny = std::clamp(pcy + dy, 0, shH-1);
                vr += _envMap[nx][ny][0];
                vg += _envMap[nx][ny][1];
                vb += _envMap[nx][ny][2];
                ++pCount;
            }
            if (pCount > 0) { vr /= pCount; vg /= pCount; vb /= pCount; }

            CachedLight vl;
            vl.dir[0]=vdx; vl.dir[1]=vdy; vl.dir[2]=vdz;
            vl.color[0]=(double)vr * _envIntensity;
            vl.color[1]=(double)vg * _envIntensity;
            vl.color[2]=(double)vb * _envIntensity;
            vl.pos[0]=vl.pos[1]=vl.pos[2]=0;
            vl.isPoint = false;

            // Transform into volume-local space if needed (matches gatherLights)
            if (_hasVolumeXform) {
                double ldx=_volInv[0][0]*vl.dir[0]+_volInv[1][0]*vl.dir[1]+_volInv[2][0]*vl.dir[2];
                double ldy=_volInv[0][1]*vl.dir[0]+_volInv[1][1]*vl.dir[1]+_volInv[2][1]*vl.dir[2];
                double ldz=_volInv[0][2]*vl.dir[0]+_volInv[1][2]*vl.dir[1]+_volInv[2][2]*vl.dir[2];
                double ll=std::sqrt(ldx*ldx+ldy*ldy+ldz*ldz);
                if(ll>1e-8){ldx/=ll;ldy/=ll;ldz/=ll;}
                vl.dir[0]=ldx; vl.dir[1]=ldy; vl.dir[2]=ldz;
            }

            _lights.push_back(vl);
        }
    }
}

void VDBRenderIop::sampleEnv(const openvdb::Vec3d& dir, float& r, float& g, float& b) const {
    double u=std::atan2(dir[0],-dir[2])/(2*M_PI)+0.5;
    double v=std::asin(std::clamp(dir[1],-1.0,1.0))/M_PI+0.5;
    // Combine EnvironLight Y rotation + manual offset
    u+=(_envLightRotY+_envRotate)/360.0;
    u-=std::floor(u);
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

// kDirs26: all 6+8+12 directions concatenated — used by ReSTIR as candidate set.
// ReSTIR weights each by L(ω)×phase(ω), selects one, traces one shadow ray.
static const openvdb::Vec3d kDirs26[] = {
    {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
    {0.577,0.577,0.577},{0.577,0.577,-0.577},{0.577,-0.577,0.577},{0.577,-0.577,-0.577},
    {-0.577,0.577,0.577},{-0.577,0.577,-0.577},{-0.577,-0.577,0.577},{-0.577,-0.577,-0.577},
    {0.707,0.707,0},{0.707,-0.707,0},{-0.707,0.707,0},{-0.707,-0.707,0},
    {0.707,0,0.707},{0.707,0,-0.707},{-0.707,0,0.707},{-0.707,0,-0.707},
    {0,0.707,0.707},{0,0.707,-0.707},{0,-0.707,0.707},{0,-0.707,-0.707}};
static constexpr int kDirs26Count = 26;

// ── [V2] Phase function helpers ──────────────────────────────────────────────

double VDBRenderIop::hgRaw(double cosT, double g) noexcept {
    if (std::abs(g) < 0.001) return 1.0;
    const double g2    = g * g;
    const double denom = 1.0 + g2 - 2.0 * g * cosT;
    return (1.0 - g2) / (denom * std::sqrt(denom));
}

double VDBRenderIop::jitterHash(int x, int y) noexcept {
    auto u = static_cast<uint32_t>(x) * 2654435761u
           ^ static_cast<uint32_t>(y) * 2246822519u;
    u ^= u >> 16; u *= 0x45d9f3bu; u ^= u >> 16;
    return (u & 0xFFFFu) / 65536.0;
}

// ── [V4] Procedural noise helpers ──────────────────────────────────────────
// Value noise with smooth gradient via hash — cheap and cache-friendly.
// Returns [-1, 1]. For density perturbation we use abs() to avoid sign flips.

double VDBRenderIop::noiseHash3(double x, double y, double z) noexcept {
    // Integer grid coords
    const int ix = (int)std::floor(x), iy = (int)std::floor(y), iz = (int)std::floor(z);
    // Fractional part with smooth Hermite curve
    double fx = x - ix, fy = y - iy, fz = z - iz;
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);
    fz = fz * fz * (3.0 - 2.0 * fz);

    // Hash function: deterministic, avoids tabling
    auto h = [](int a, int b, int c) -> double {
        uint32_t u = static_cast<uint32_t>(a) * 1664525u
                   ^ static_cast<uint32_t>(b) * 1013904223u
                   ^ static_cast<uint32_t>(c) * 2246822519u;
        u ^= u >> 16; u *= 0x45d9f3bu; u ^= u >> 16;
        return (int)(u & 0xFFFFu) / 32768.0 - 1.0;
    };

    // Trilinear interpolation of corner values
    double v000=h(ix,iy,iz),     v100=h(ix+1,iy,iz);
    double v010=h(ix,iy+1,iz),   v110=h(ix+1,iy+1,iz);
    double v001=h(ix,iy,iz+1),   v101=h(ix+1,iy,iz+1);
    double v011=h(ix,iy+1,iz+1), v111=h(ix+1,iy+1,iz+1);

    return v000 + fx*(v100-v000)
         + fy*(v010-v000 + fx*(v110-v010-v100+v000))
         + fz*(v001-v000 + fx*(v101-v001-v100+v000)
               + fy*(v011-v001-v010+v000
                    + fx*(v111-v011-v101+v001-v110+v010+v100-v000)));
}

double VDBRenderIop::noiseFBm(double x, double y, double z,
                               int octaves, double roughness) noexcept {
    double val = 0.0, amp = 1.0, total = 0.0;
    double freq = 1.0;
    for (int i = 0; i < octaves; ++i) {
        val   += amp * noiseHash3(x * freq, y * freq, z * freq);
        total += amp;
        amp   *= roughness;
        freq  *= 2.0;
    }
    return (total > 0.0) ? val / total : 0.0;
}

// ── [V4] Approximate Mie phase function ──────────────────────────────────
// Jendersie & d'Eon, SIGGRAPH 2023 Talks:
// "An Approximate Mie Scattering Function for Fog and Cloud Rendering"
// Parametric fit to Lorenz-Mie for spherical water droplets.
// d = droplet diameter in micrometres.
//   d ≈ 0.1  → aerosol / haze
//   d ≈ 2    → cloud droplets
//   d ≈ 10   → large cloud / drizzle
// Returns phS = phase × 4π, same convention as hgRaw().

double VDBRenderIop::miePhaseS(double cosT, double d) noexcept {
    // Fitted HG asymmetry parameters from Jendersie & d'Eon Table 1
    // (valid for d in [0.1, 50] µm, λ = 550 nm)
    const double d2   = d * d;
    const double d3   = d2 * d;
    // g₁: dominant forward lobe
    const double g1   = std::clamp(0.5 * d / (d + 1.0), 0.01, 0.99);
    // g₂: secondary backward lobe (captures Mie's distinctive backward peak)
    const double g2   = std::clamp(-0.12 * std::exp(-0.09 * d), -0.99, -0.01);
    // α: weight of g₁ lobe
    const double alpha = std::clamp(0.7 - 0.06 * d + 0.002 * d2, 0.0, 1.0);

    // Evaluate weighted two-lobe HG
    double phS = alpha * hgRaw(cosT, g1) + (1.0 - alpha) * hgRaw(cosT, g2);

    // Mie has a distinctive narrow forward peak for larger droplets.
    // Add a sharp Cornette-Shanks correction term for d > 2µm.
    if (d > 2.0) {
        const double gcs = std::clamp(0.9 - 0.015 * d, 0.5, 0.98);
        // Cornette-Shanks: p(θ) ∝ (1-g²)(1+cos²θ) / (2+g²)^(3/2) / (1+g²-2g·cosθ)^(3/2)
        const double g2cs  = gcs * gcs;
        const double denom = 1.0 + g2cs - 2.0 * gcs * cosT;
        const double cs    = (3.0 / (8.0 * M_PI))
                           * ((1.0 - g2cs) * (1.0 + cosT * cosT))
                           / ((2.0 + g2cs) * std::pow(denom, 1.5));
        // cs is normalised; convert to phS convention (* 4π)
        const double csW = std::clamp((d - 2.0) * 0.1, 0.0, 0.3);
        phS = (1.0 - csW) * phS + csW * cs * (4.0 * M_PI);
    }

    return phS;
}

// ── [V5] L2 Spherical Harmonic irradiance evaluation ─────────────────────
// Evaluates the L2 SH approximation of the environment at direction (dx,dy,dz).
// sh9 = 9 coefficients in zonal/real SH order [l=0,l=1×3,l=2×5].
// Ramamoorthi & Hanrahan (2001): 9 coefficients capture >97% of diffuse energy.
// No shadow rays needed — SH represents the full-sphere ambient integral.
// Returns clamped irradiance for one colour channel.

double VDBRenderIop::evalEnvSH(const double sh9[9],
                                double dx, double dy, double dz) noexcept {
    // L0
    double v = 0.282095 * sh9[0];
    // L1
    v += 0.488603 * dy * sh9[1];
    v += 0.488603 * dz * sh9[2];
    v += 0.488603 * dx * sh9[3];
    // L2
    v += 1.092548 * dx * dy * sh9[4];
    v += 1.092548 * dy * dz * sh9[5];
    v += 0.315392 * (3.0*dz*dz - 1.0) * sh9[6];
    v += 1.092548 * dx * dz * sh9[7];
    v += 0.546274 * (dx*dx - dy*dy) * sh9[8];
    return std::max(0.0, v);
}

// ═══ [V3] Shadow transmittance helper ════════════════════════════════════════
// Three code paths in priority order:
//   1. Cache:    single BoxSampler lookup — O(1), directional lights only
//   2. HDDA:     skip empty VDB tiles, step through active intervals
//   3. Uniform:  original march — fallback when neither is available
//
// lightIdx: index into ctx.shadowCacheAcc (pass -1 for env map directions)

double VDBRenderIop::evalShadowTransmittance(
        MarchCtx&                        ctx,
        const openvdb::math::Transform&  xf,
        const openvdb::Vec3d&            wP,
        const openvdb::Vec3d&            lD,
        double ext, double shDen,
        int lightIdx,
        const openvdb::Vec3d& bboxMin,
        const openvdb::Vec3d& bboxMax,
        int nSh, double shStep)
{
    // ── Path 1: Transmittance cache ──────────────────────────────────────
    if (ctx.useShadowCache
        && lightIdx >= 0
        && lightIdx < (int)ctx.shadowCacheAcc.size()
        && ctx.shadowCacheAcc[lightIdx]) {
        const auto iP = xf.worldToIndex(wP);
        float T = openvdb::tools::BoxSampler::sample(*ctx.shadowCacheAcc[lightIdx], iP);
        return (double)std::clamp(T, 0.0f, 1.0f);
    }

    double lT = 1.0;

    // ── Path 2: HDDA shadow ray ──────────────────────────────────────────
    if (ctx.shadowRI) {
        // Small offset avoids self-intersection with the current march step.
        const openvdb::Vec3d shadowOrigin = wP + shStep * 0.5 * lD;
        openvdb::math::Ray<double> sRay(shadowOrigin, lD);
        if (ctx.shadowRI->setWorldRay(sRay)) {
            double it0, it1;
            while (ctx.shadowRI->march(it0, it1) && lT > 0.01) {
                const auto wsS = ctx.shadowRI->getWorldPos(it0);
                const auto wsE = ctx.shadowRI->getWorldPos(it1);
                double wt0 = (wsS - shadowOrigin).dot(lD);
                double wt1 = (wsE - shadowOrigin).dot(lD);
                if (wt1 <= 0.0) continue;
                if (wt0 < 0.0)  wt0 = 0.0;
                for (double ts = wt0; ts < wt1 && lT > 0.01; ts += shStep) {
                    const auto lwP = shadowOrigin + ts * lD;
                    bool in = true;
                    for (int a = 0; a < 3; ++a)
                        if (lwP[a] < bboxMin[a] || lwP[a] > bboxMax[a]) { in = false; break; }
                    if (!in) break;
                    const auto liP = xf.worldToIndex(lwP);
                    lT *= std::exp(-(double)ctx.sampleShadow(liP) * ext * shDen * shStep);
                }
            }
        }
        return lT;
    }

    // ── Path 3: Uniform march fallback ───────────────────────────────────
    for (int i = 0; i < nSh; ++i) {
        const auto lw = wP + ((i + 1) * shStep) * lD;
        bool in = true;
        for (int a = 0; a < 3; ++a)
            if (lw[a] < bboxMin[a] || lw[a] > bboxMax[a]) { in = false; break; }
        if (!in) break;
        const auto li = xf.worldToIndex(lw);
        lT *= std::exp(-(double)ctx.sampleShadow(li) * ext * shDen * shStep);
        if (lT < 0.01) break;
    }
    return lT;
}

// ═══ Per-scanline march context ═══

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

    // ── [V4] Procedural detail noise ──
    c.noiseEnable    = _noiseEnable;
    c.noiseScale     = std::max(0.001, _noiseScale);
    c.noiseStrength  = std::clamp(_noiseStrength, 0.0, 1.0);
    c.noiseOctaves   = std::clamp(_noiseOctaves, 1, 6);
    c.noiseRoughness = std::clamp(_noiseRoughness, 0.0, 1.0);

    // ── [V4] Phase function mode ──
    c.phaseMode   = _phaseMode;
    c.mieDropletD = std::max(0.1, _mieDropletD);

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

    // ── [V5] SH environment lighting ──
    c.envMode  = _envMode;
    c.hasEnvSH = _hasEnvSH;
    if (_hasEnvSH)
        std::memcpy(c.envSH, _envSH, sizeof(_envSH));
    c.useReSTIR = _useReSTIR;

    // ── Grid accessors ──
    if (_hasTempGrid  && _tempGrid)  c.tempAcc  = std::make_unique<openvdb::FloatGrid::ConstAccessor>(_tempGrid->getConstAccessor());
    if (_hasFlameGrid && _flameGrid) c.flameAcc = std::make_unique<openvdb::FloatGrid::ConstAccessor>(_flameGrid->getConstAccessor());
    if (_hasVelGrid   && _velGrid)   c.velAcc   = std::make_unique<openvdb::Vec3SGrid::ConstAccessor>(_velGrid->getConstAccessor());
    if (_hasColorGrid && _colorGrid) c.colorAcc = std::make_unique<openvdb::Vec3SGrid::ConstAccessor>(_colorGrid->getConstAccessor());

    // ── [V3] Shadow HDDA — shallow copy of _volRI ──
    // setWorldRay() resets HDDA state between shadow rays so this copy
    // can be reused for every shadow cast within a single scanline.
    if (_volRI) c.shadowRI = std::make_unique<VRI>(*_volRI);

    // ── [V3] Shadow cache accessors ──
    c.useShadowCache = _useShadowCache && !_shadowCaches.empty();
    if (c.useShadowCache) {
        c.shadowCacheAcc.resize(_shadowCaches.size());
        for (size_t li = 0; li < _shadowCaches.size(); ++li) {
            if (_shadowCaches[li].valid && _shadowCaches[li].transGrid)
                c.shadowCacheAcc[li] = std::make_unique<openvdb::FloatGrid::ConstAccessor>(
                    _shadowCaches[li].transGrid->getConstAccessor());
        }
        // ── [V6] Map env axis dirs to their cache indices ──
        for (int di = 0; di < 6; ++di)
            c.envDirCacheIdx[di] = (_envDirCacheBase >= 0)
                ? (_envDirCacheBase + di)
                : -1;
    }

#ifdef VDBRENDER_HAS_NEURAL
    if (_neuralMode && _neural && _neural->loaded()) {
        c.neuralDec  = _neural.get();
        c.neuralMode = true;
    }
#endif
    return c;
}

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

        // ── [V4] Procedural detail noise ─────────────────────────────────
        // fBm perturbs density at render time, adding micro-detail beyond
        // VDB resolution. Clamped to [0, ∞) — never makes density negative.
        // World-space coordinates scaled by noiseScale / bbox diagonal.
        if (ctx.noiseEnable && density > 1e-6f) {
            const double bboxDiag = (_bboxMax - _bboxMin).length();
            const double ns = ctx.noiseScale / (bboxDiag > 1e-8 ? bboxDiag : 1.0);
            const double n = noiseFBm(wP[0]*ns, wP[1]*ns, wP[2]*ns,
                                       ctx.noiseOctaves, ctx.noiseRoughness);
            // n is in [-1,1]; map to [1-strength, 1+strength] then multiply
            const double noiseAmp = 1.0 + ctx.noiseStrength * n;
            density = (float)std::max(0.0, (double)density * noiseAmp);
        }

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

            // ── [V4] Phase function — dual-lobe HG or approximate Mie ──
            const double cosT = -(dir[0]*lD[0] + dir[1]*lD[1] + dir[2]*lD[2]);
            double phS;
            if (ctx.phaseMode == 1) {
                // Approximate Mie (Jendersie & d'Eon 2023)
                phS = miePhaseS(cosT, ctx.mieDropletD);
            } else {
                // Dual-lobe Henyey-Greenstein (V2 default)
                phS = ctx.lobeMix       * hgRaw(cosT, ctx.gFwd)
                    + (1.0-ctx.lobeMix) * hgRaw(cosT, ctx.gBck);
            }

            // ── Gradient-normal Lambertian blend ──
            // Blends HG (view-relative) with Lambertian NdotL (light-relative).
            // Gives sculpted, three-dimensional appearance to cloud edges.
            if (hasGrad) {
                const double NdotL = std::max(0.0, gx*lD[0] + gy*lD[1] + gz*lD[2]);
                phS = (1.0 - ctx.gradMix) * phS + ctx.gradMix * NdotL;
            }

            // ── Powder modulation ──
            phS *= powder;

            // ── Shadow transmittance ──
            const double lT = evalShadowTransmittance(
                ctx, xf, wP, lD, ext, _shadowDensity,
                (int)(&lt - _lights.data()),
                _bboxMin, _bboxMax, nSh, shStep);

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

            if (ctx.envMode == 1 && ctx.hasEnvSH) {
                // ── [V5] SH path: 6-dir stratified sample + HDDA shadows ──────
                // The flat-light problem with pure L0: uniform dirs casts shadow
                // rays per direction, so dense volumes get directional shadowing
                // from the sky. L0-only has no shadow → uniformly lit.
                //
                // Fix: evaluate SH at the 6 axis dirs, cast HDDA shadow ray per
                // dir, accumulate weighted. Cost: 6 SH evals + 6 HDDA shadow
                // rays (no fixed step count — HDDA exits empty space instantly).
                // Still ~10-20x faster than 26 dirs × 16 uniform shadow steps.

                // Separate SH bands into per-channel arrays
                double shR[9], shG[9], shB[9];
                for (int i = 0; i < 9; ++i) {
                    shR[i] = ctx.envSH[i][0];
                    shG[i] = ctx.envSH[i][1];
                    shB[i] = ctx.envSH[i][2];
                }

                // 6 stratified axis directions cover the sphere evenly
                // Solid angle weight per dir = 4π/6
                constexpr double kW6 = (4.0 * M_PI) / 6.0;
                double envAccR = 0, envAccG = 0, envAccB = 0;

                for (int di = 0; di < 6; ++di) {
                    const openvdb::Vec3d& eDir = kDirs6[di];

                    // SH radiance in this direction (9 MADs per channel)
                    const double lR = evalEnvSH(shR, eDir[0], eDir[1], eDir[2]);
                    const double lG = evalEnvSH(shG, eDir[0], eDir[1], eDir[2]);
                    const double lB = evalEnvSH(shB, eDir[0], eDir[1], eDir[2]);

                    // Skip near-black directions (saves shadow ray)
                    const double lum = 0.2126*lR + 0.7152*lG + 0.0722*lB;
                    if (lum < 1e-4) continue;

                    // [V6] Use precomputed transmittance cache if available,
                    // otherwise fall back to HDDA shadow ray.
                    openvdb::Vec3d wDir = eDir;
                    if (_hasVolumeXform) {
                        wDir = openvdb::Vec3d(
                            _volFwd[0][0]*eDir[0]+_volFwd[1][0]*eDir[1]+_volFwd[2][0]*eDir[2],
                            _volFwd[0][1]*eDir[0]+_volFwd[1][1]*eDir[1]+_volFwd[2][1]*eDir[2],
                            _volFwd[0][2]*eDir[0]+_volFwd[1][2]*eDir[1]+_volFwd[2][2]*eDir[2]);
                        const double wl = wDir.length();
                        if (wl > 1e-8) wDir /= wl;
                    }
                    const double eT = evalShadowTransmittance(
                        ctx, xf, wP, wDir, ext, _shadowDensity,
                        ctx.envDirCacheIdx[di],   // O(1) cache hit when available
                        _bboxMin, _bboxMax, nSh, shStep);

                    envAccR += lR * eT;
                    envAccG += lG * eT;
                    envAccB += lB * eT;
                }

                const double envBase = ss * step * _envIntensity * kW6;
                aR += envBase * TR * envAccR;
                aG += envBase * TG * envAccG;
                aB += envBase * TB * envAccB;
                stepR += envBase * envAccR;
                stepG += envBase * envAccG;
                stepB += envBase * envAccB;

            } else {
                // ── Uniform dirs / ReSTIR path ────────────────────────────────
                const int nEnv = 6 + (int)(std::clamp(_envDiffuse, 0.0, 1.0) * 20);

                if (ctx.useReSTIR && ctx.hasEnvSH) {
                    // ── [V6] ReSTIR: weight all 26 dirs, shadow-trace one ─────
                    // Build reservoir: each candidate weighted by L(ω) × phase(ω).
                    // Phase-weighted importance means the selected direction has
                    // the highest expected contribution — one shadow ray, full quality.
                    Reservoir res;

                    // Deterministic per-step hash for reservoir selection
                    // XOR step count into the jitter hash for uncorrelated seeds
                    const uint32_t px2 = static_cast<uint32_t>(wP[0]*1000)
                                       ^ static_cast<uint32_t>(wP[1]*1000+0.5) * 2654435761u
                                       ^ static_cast<uint32_t>(wP[2]*1000+0.3) * 2246822519u;
                    auto pcgR = [](uint32_t u) -> float {
                        u ^= u>>16; u*=0x45d9f3bu; u^=u>>16;
                        return (u & 0xFFFFu) / 65536.0f;
                    };
                    uint32_t seed = px2;

                    // Separate SH into per-channel arrays (reuse if already done above)
                    double shR[9], shG[9], shB[9];
                    for (int i=0; i<9; ++i) {
                        shR[i]=ctx.envSH[i][0]; shG[i]=ctx.envSH[i][1]; shB[i]=ctx.envSH[i][2]; }

                    for (int di = 0; di < kDirs26Count; ++di) {
                        const openvdb::Vec3d& eDir = kDirs26[di];
                        // Target function: L(ω) × phase(ω) — importance weight
                        const double lR = std::max(0.0, evalEnvSH(shR, eDir[0], eDir[1], eDir[2]));
                        const double lG = std::max(0.0, evalEnvSH(shG, eDir[0], eDir[1], eDir[2]));
                        const double lB = std::max(0.0, evalEnvSH(shB, eDir[0], eDir[1], eDir[2]));
                        const double cosT2 = -(dir[0]*eDir[0]+dir[1]*eDir[1]+dir[2]*eDir[2]);
                        double ph2;
                        if (ctx.phaseMode==1) ph2 = miePhaseS(cosT2, ctx.mieDropletD);
                        else ph2 = ctx.lobeMix*hgRaw(cosT2,ctx.gFwd)+(1.0-ctx.lobeMix)*hgRaw(cosT2,ctx.gBck);
                        const float w2 = (float)((0.2126*lR+0.7152*lG+0.0722*lB) * ph2);
                        seed ^= (uint32_t)(di*2654435761u);
                        res.update(di, w2, pcgR(seed));
                    }

                    // Shadow-trace the selected direction only
                    if (res.dirIdx >= 0 && res.W() > 1e-6f) {
                        const openvdb::Vec3d& eDir = kDirs26[res.dirIdx];
                        openvdb::Vec3d wDir = eDir;
                        if (_hasVolumeXform) {
                            wDir = openvdb::Vec3d(
                                _volFwd[0][0]*eDir[0]+_volFwd[1][0]*eDir[1]+_volFwd[2][0]*eDir[2],
                                _volFwd[0][1]*eDir[0]+_volFwd[1][1]*eDir[1]+_volFwd[2][1]*eDir[2],
                                _volFwd[0][2]*eDir[0]+_volFwd[1][2]*eDir[1]+_volFwd[2][2]*eDir[2]);
                            const double wl2=wDir.length(); if(wl2>1e-8) wDir/=wl2;
                        }
                        const double eT = evalShadowTransmittance(
                            ctx, xf, wP, wDir, ext, _shadowDensity,
                            -1, _bboxMin, _bboxMax, nSh, shStep);

                        float eRc, eGc, eBc;
                        sampleEnv(wDir, eRc, eGc, eBc);

                        // Unbiased estimate: contrib × W × nEnv (matches uniform energy)
                        const double envBase = ss * eT * step * _envIntensity
                                             * (double)res.W() * nEnv;
                        aR += envBase * TR * eRc;
                        aG += envBase * TG * eGc;
                        aB += envBase * TB * eBc;
                        stepR += envBase * eRc;
                        stepG += envBase * eGc;
                        stepB += envBase * eBc;
                    }

                } else {
                    // ── Original uniform dirs ─────────────────────────────────
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
                        const double eT = evalShadowTransmittance(
                            ctx, xf, wP, eDir, ext, _shadowDensity,
                            -1, _bboxMin, _bboxMax, nSh, shStep);
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

// ═══ marchRayDensity — ramp modes (HDDA + trilinear) ═══

void VDBRenderIop::marchRayDensity(MarchCtx&ctx,const openvdb::Vec3d&origin,const openvdb::Vec3d&dir,float&outD,float&outA) const {
    outD=0;outA=0;if(!_floatGrid)return;
    auto&acc=ctx.densAcc;const auto&xf=_floatGrid->transform();
    bool useTG=_hasTempGrid&&_tempGrid&&_colorScheme==kBlackbody;
    auto*tA=ctx.tempAcc.get();
    double step=ctx.step,ext=ctx.ext,T=1,wV=0;

    auto shadeDensity=[&](const openvdb::Vec3d&iP)->float{
        float d=ctx.sampleDensity(iP)*(float)_densityMix;
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
        while(vri.march(it0,it1)&&T>.005){
            auto wS=vri.getWorldPos(it0),wE=vri.getWorldPos(it1);
            double wT0=(wS-origin).dot(dir),wT1=(wE-origin).dot(dir);
            if(wT1<=0)continue;if(wT0<0)wT0=0;
            for(double t2=wT0;t2<wT1&&T>.005;){
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
        for(double t2=tEnter;t2<tExit&&T>.005;){
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

                    if(dAcc){
#ifdef VDBRENDER_HAS_NEURAL
                        float density=(_neuralMode&&_neural)?_neural->sampleDensity(iP)*(float)_densityMix:dAcc->getValue(ijk)*(float)_densityMix;
#else
                        float density=dAcc->getValue(ijk)*(float)_densityMix;
#endif
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
                                if(_floatGrid){
#ifdef VDBRENDER_HAS_NEURAL
                                    if(_neuralMode&&_neural){
                                        for(int i=0;i<nSh;++i){auto lw=wP+((i+1)*shStep)*lD;bool in=true;
                                            for(int a2=0;a2<3;++a2)if(lw[a2]<_bboxMin[a2]||lw[a2]>_bboxMax[a2]){in=false;break;}if(!in)break;
                                            auto li=xf.worldToIndex(lw);
                                            lT*=std::exp(-(double)_neural->sampleDensity(li)*ext*_shadowDensity*shStep);
                                            if(lT<.01)break;}
                                    }else
#endif
                                    {auto la=_floatGrid->getConstAccessor();
                                    for(int i=0;i<nSh;++i){auto lw=wP+((i+1)*shStep)*lD;bool in=true;
                                        for(int a2=0;a2<3;++a2)if(lw[a2]<_bboxMin[a2]||lw[a2]>_bboxMax[a2]){in=false;break;}if(!in)break;
                                        auto li=xf.worldToIndex(lw);
                                        lT*=std::exp(-(double)la.getValue(openvdb::Coord((int)std::floor(li[0]),(int)std::floor(li[1]),(int)std::floor(li[2])))*ext*_shadowDensity*shStep);
                                        if(lT<.01)break;}}
                                }
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
