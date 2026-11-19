#pragma once
// ChromaStudio Engine · render_gl · GLSL 3.30 sources.
//
// kFullscreenVertex (§9.1) and kStampFragment (§4.1) are the normative listings, unchanged.
// kCompositeFragment (§7.1), kWetFragment (§5.1) and the §9.2–§9.6 filters are normative listings,
// unchanged. The others are the minimum the dry
// pipeline and the layer tree need around them:
//   dab composite  build mode: source-over of (inkPma·w) into the working tile, or the eraser (1−w)·P;
//                  under Alpha Lock (a0·mix(straight(P), ink, w), a0) with a0 from the stroke's base
//   resolve        max mode: before ∘ (inkPma · max-weight), or (1−w)·before for the eraser; the same
//                  Alpha Lock rule
//   unit coverage  a clip base at unit coverage: (straight(base), 1) (§7 Clipping Mask)
//   clip           the composed chain cut back to the base silhouette aBase·opacityBase
//   reservoir      the surface under a wet stamp, read back once per stamp (§5)
//   blur mix       the Blur Brush result mix(S, blur, w); lock apply: Alpha Lock for wet output (§7)
//   display        one composed tile onto the window over the paper, sRGB-encoded once (§2)
//   ring           brush outline at the cursor (§11 hover contour)
// All tile images share origin and size; GL row 0 of a tile is its logical top (§7.1).

namespace chroma::gl::shaders {

// §9.1 — glDrawArrays(GL_TRIANGLES, 0, 3) with the VAO bound.
inline constexpr const char* kFullscreenVertex = R"glsl(#version 330 core
void main() {
    vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)glsl";

// §4.1 — weight of one stamp into R16F.
inline constexpr const char* kStampFragment = R"glsl(#version 330 core
uniform sampler2D selection, pattern, tipMask;
uniform bool useTipMask;
uniform ivec2 originPx;
uniform vec2 centerPx;
uniform float radius, aspect, hardness, angle, inkWeight;
uniform float grainPx, grainAmount, periodPx, density, toneAngle;
uniform int kind;
uniform uint seed;
layout(location=0) out float weight;
vec2 rotateBack(vec2 p,float a) {
    float c=cos(a), s=sin(a);
    return vec2(c*p.x+s*p.y,-s*p.x+c*p.y);
}
float noiseAt(ivec2 p) {
    uint n=uint(p.x)*0x9E3779B1u ^ uint(p.y)*0x85EBCA77u ^ seed;
    n^=n>>16u; n*=0x7FEB352Du; n^=n>>15u; n*=0x846CA68Bu; n^=n>>16u;
    return float(n>>8u)/16777215.0;
}
float paper(vec2 p) {
    vec2 q=p/max(.25,grainPx), f=fract(q);
    f=f*f*(3.0-2.0*f);
    ivec2 i=ivec2(floor(q));
    float n=mix(mix(noiseAt(i),noiseAt(i+ivec2(1,0)),f.x),
                mix(noiseAt(i+ivec2(0,1)),noiseAt(i+ivec2(1)),f.x),f.y);
    return mix(1.0,n,clamp(grainAmount,0.0,1.0))*
           textureLod(pattern,p/vec2(textureSize(pattern,0)),0.0).r;
}
float tone(vec2 p) {
    if(density<=0.0) return 0.0;
    if(density>=1.0) return 1.0;
    bool holes=density>.5;
    float rr=(holes?1.0-density:density)/3.141592653589793;
    float n=0.0;
    for(int y=0;y<4;++y) for(int x=0;x<4;++x) {
        vec2 offset=(vec2(x,y)+.5)/4.0-.5;
        vec2 uv=rotateBack(p+offset,toneAngle)/max(2.0,periodPx);
        vec2 q=fract(uv+(holes?.5:0.0))-.5;
        bool inside=dot(q,q)<=rr;
        n+=(holes!=inside)?1.0:0.0;
    }
    return n/16.0;
}
void main() {
    if(radius<=0.0) { weight=0.0; return; }
    ivec2 at=ivec2(gl_FragCoord.xy);
    vec2 p=vec2(originPx+at)+.5;
    float minor=radius*clamp(aspect,.05,1.0);
    vec2 local=rotateBack(p-centerPx,angle)/vec2(radius,minor);
    float rho=length(local);
    float w=min(1.0,.5/max(.125,minor));
    float cover=1.0-smoothstep(1.0-w,1.0+w,rho);
    if(kind==1) cover*=exp(-4.5*rho*rho);
    else if(hardness<1.0) cover*=1.0-smoothstep(clamp(hardness,0.0,1.0),1.0,rho);
    if(useTipMask) {
        vec2 uv=local*.5+.5;
        bool inside=all(greaterThanEqual(uv,vec2(0))) && all(lessThanEqual(uv,vec2(1)));
        cover*=inside ? textureLod(tipMask,uv,0.0).r : 0.0;
    }
    if(kind==2) cover*=paper(p);
    if(kind==3) cover*=tone(p);
    weight=clamp(cover*inkWeight*texelFetch(selection,at,0).r,0.0,1.0);
}
)glsl";

// Build mode, one stamp: the weight just written by kStampFragment becomes the source alpha.
// Paint: blend (ONE, ONE_MINUS_SRC_ALPHA) gives source-over in PMA. Eraser: blend
// (ZERO, ONE_MINUS_SRC_ALPHA) gives P′ = (1 − w)·P including alpha (§4).
// Alpha Lock (§7): blend (ONE, ONE_MINUS_SRC_ALPHA) for RGB and (ZERO, ONE) for alpha, so
// P' = (a0·w·ink.rgb + (1 − ink.a·w)·P.rgb, P.a) = (a0·mix(straight(P), ink, ink.a·w), a0); the eraser
// writes nothing.
inline constexpr const char* kDabCompositeFragment = R"glsl(#version 330 core
uniform sampler2D weightTex, lockTex;
uniform vec4 inkPma;
uniform bool erase, alphaLock;
layout(location=0) out vec4 pixel;
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
    float w=texelFetch(weightTex,at,0).r;
    if(alphaLock) {
        float a0=texelFetch(lockTex,at,0).a;
        pixel=erase ? vec4(0.0) : vec4(inkPma.rgb*(w*a0),inkPma.a*w);
        return;
    }
    pixel=erase ? vec4(0.0,0.0,0.0,w) : inkPma*w;
}
)glsl";

// Max mode, on commit: the stroke's per-pixel maximum weight applied once over the base (§4).
inline constexpr const char* kResolveFragment = R"glsl(#version 330 core
uniform sampler2D baseTex, weightTex;
uniform bool hasBase, erase, alphaLock;
uniform vec4 inkPma;
layout(location=0) out vec4 pixel;
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
    vec4 base=hasBase ? texelFetch(baseTex,at,0) : vec4(0.0);
    float w=texelFetch(weightTex,at,0).r;
    if(alphaLock) {
        pixel=erase ? base : vec4(inkPma.rgb*(w*base.a)+base.rgb*(1.0-inkPma.a*w),base.a);
        return;
    }
    pixel=erase ? base*(1.0-w) : inkPma*w+base*(1.0-inkPma.a*w);
}
)glsl";

// A screen-space quad: corners in NDC (x0, yTop, x1, yBottom) and texture coordinates
// (u0, vTop, u1, vBottom). Drawn as a 4-vertex triangle strip.
inline constexpr const char* kQuadVertex = R"glsl(#version 330 core
uniform vec4 rectNdc;
uniform vec4 uvRect;
out vec2 uv;
void main() {
    vec2 c=vec2(gl_VertexID&1,gl_VertexID>>1);
    gl_Position=vec4(mix(rectNdc.x,rectNdc.z,c.x),mix(rectNdc.y,rectNdc.w,c.y),0.0,1.0);
    uv=vec2(mix(uvRect.x,uvRect.z,c.x),mix(uvRect.y,uvRect.w,c.y));
}
)glsl";

// One composed tile on screen over the opaque paper, then one sRGB encode (§2). Texture row 0 is the
// top; the quad flips Y once.
inline constexpr const char* kDisplayFragment = R"glsl(#version 330 core
uniform sampler2D layerTex;
uniform vec3 paper;
uniform float opacity;
in vec2 uv;
layout(location=0) out vec4 pixel;
vec3 encode(vec3 c) {
    c=clamp(c,0.0,1.0);
    return mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,step(vec3(0.0031308),c));
}
void main() {
    vec4 p=texture(layerTex,uv)*clamp(opacity,0.0,1.0);
    pixel=vec4(encode(p.rgb+paper*(1.0-p.a)),1.0);
}
)glsl";

// §7.1 — the compositor, unchanged.
inline constexpr const char* kCompositeFragment = R"glsl(#version 330 core
uniform sampler2D bottomImage, topImage;
uniform float opacity;
uniform int mode;
layout(location=0) out vec4 pixel;
float sat(float x) { return clamp(x,0.0,1.0); }
float lum(vec3 v) { return dot(v,vec3(.30,.59,.11)); }
float span3(vec3 v) { return max(max(v.r,v.g),v.b)-min(min(v.r,v.g),v.b); }
vec3 withSat(vec3 c,float target) {
    float d=span3(c);
    return d>0.0 ? (c-min(min(c.r,c.g),c.b))*(target/d) : vec3(0);
}
vec3 withLum(vec3 c,float target) {
    c+=target-lum(c);
    float n=min(min(c.r,c.g),c.b), x=max(max(c.r,c.g),c.b);
    if(n<0.0) c=target+(c-target)*(target/(target-n));
    if(x>1.0) c=target+(c-target)*((1.0-target)/(x-target));
    return clamp(c,0.0,1.0);
}
float dodge(float b,float s) {
    if(b==0.0) return 0.0;
    return s>=1.0 ? 1.0 : min(1.0,b/(1.0-s));
}
float burn(float b,float s) {
    if(b>=1.0) return 1.0;
    return s<=0.0 ? 0.0 : 1.0-min(1.0,(1.0-b)/s);
}
float overlay(float b,float s) {
    return b<=.5 ? 2.0*b*s : 1.0-2.0*(1.0-b)*(1.0-s);
}
float vivid(float b,float s) {
    return s<.5 ? burn(b,2.0*s) : dodge(b,2.0*s-1.0);
}
float channel(float b,float s) {
    switch(mode) {
        case 1: return b*s;
        case 2: return b+s-b*s;
        case 3: return overlay(b,s);
        case 4: return min(1.0,b+s);
        case 5: return dodge(b,s);
        case 6: return max(0.0,b+s-1.0);
        case 7: return min(b,s);
        case 8: return max(b,s);
        case 9: return burn(b,s);
        case 12: {
            float d=b<=.25 ? ((16.0*b-12.0)*b+4.0)*b : sqrt(b);
            return s<=.5 ? b-(1.0-2.0*s)*b*(1.0-b) : b+(2.0*s-1.0)*(d-b);
        }
        case 13: return overlay(s,b);
        case 14: return vivid(b,s);
        case 15: return sat(b+2.0*s-1.0);
        case 16: return s<.5 ? min(b,2.0*s) : max(b,2.0*s-1.0);
        case 17: return step(.5,vivid(b,s));
        case 18: return abs(b-s);
        case 19: return b+s-2.0*b*s;
        case 20: return max(0.0,b-s);
        case 21: return s==0.0 ? (b==0.0 ? 0.0 : 1.0) : min(b/s,1.0);
        case 26: return 1.0-b;
        default: return s;
    }
}
vec3 blend(vec3 b,vec3 s) {
    switch(mode) {
        case 10: return lum(s)<lum(b) ? s : b;
        case 11: return lum(s)>lum(b) ? s : b;
        case 22: return withLum(withSat(s,span3(b)),lum(b));
        case 23: return withLum(withSat(b,span3(s)),lum(b));
        case 24: return withLum(s,lum(b));
        case 25: return withLum(b,lum(s));
        default: return vec3(channel(b.r,s.r),channel(b.g,s.g),channel(b.b,s.b));
    }
}
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
    vec4 dst=texelFetch(bottomImage,at,0), src=texelFetch(topImage,at,0);
    float a=src.a*sat(opacity), h=dst.a;
    if(a<=0.0) { pixel=dst; return; }
    vec3 s=clamp(src.rgb/src.a,0.0,1.0), b=h>0.0?clamp(dst.rgb/h,0.0,1.0):vec3(0);
    float outA=a+h*(1.0-a);
    vec3 rgb=(1.0-a)*dst.rgb+a*(1.0-h)*s+a*h*blend(b,s);
    pixel=vec4(clamp(rgb,0.0,outA),outA);
}
)glsl";

// §7 Clipping Mask, first step: the base colour at unit coverage.
inline constexpr const char* kUnitCoverageFragment = R"glsl(#version 330 core
uniform sampler2D image;
layout(location=0) out vec4 pixel;
void main() {
    vec4 s=texelFetch(image,ivec2(gl_FragCoord.xy),0);
    pixel=s.a>0.0 ? vec4(clamp(s.rgb/s.a,0.0,1.0),1.0) : vec4(0.0,0.0,0.0,1.0);
}
)glsl";

// §7 Clipping Mask, last step: the opaque chain times the base silhouette aBase·opacityBase.
inline constexpr const char* kClipFragment = R"glsl(#version 330 core
uniform sampler2D chainImage, baseImage;
uniform float opacity;
layout(location=0) out vec4 pixel;
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
    float k=texelFetch(baseImage,at,0).a*clamp(opacity,0.0,1.0);
    pixel=vec4(texelFetch(chainImage,at,0).rgb*k,k);
}
)glsl";

// §5.1 — one wet stamp (watercolor and smudge), unchanged. imageIn is the assembled working region of
// the layer with its halo (LINEAR, CLAMP_TO_EDGE), brushMask the §4.1 weight m·o·flow on the same grid.
inline constexpr const char* kWetFragment = R"glsl(#version 330 core
uniform sampler2D imageIn, brushMask;
uniform vec2 deltaPx;
uniform vec4 reservoir, freshInk;
uniform float radius, blending, dilution, bleed, strength;
uniform bool watercolor, emitSource;
layout(location=0) out vec4 pixel;
vec4 carried(vec2 p,vec2 delta,vec2 sizePx) {
    vec4 sum=vec4(0);
    for(int k=1;k<=15;++k) {
        vec2 center=p+vec2(.5)-delta*(float(k)/15.0);
        sum+=textureLod(imageIn,center/sizePx,0.0);
    }
    return sum/15.0;
}
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
    vec4 src=texelFetch(imageIn,at,0);
    float w=clamp(texelFetch(brushMask,at,0).r*strength,0.0,1.0);
    if(w==0.0) { pixel=watercolor && emitSource ? vec4(0) : src; return; }
    vec2 p=vec2(at), dims=vec2(textureSize(imageIn,0));
    float dist=length(deltaPx), e=clamp(bleed,0.0,1.0);
    vec2 normal=dist>0.0 ? vec2(-deltaPx.y,deltaPx.x)/dist : vec2(0,1);
    vec2 side=normal*(.5*radius*e);
    vec4 u=carried(p,deltaPx,dims);
    if(e>0.0) u=.5*u+.25*(carried(p+side,deltaPx,dims)+carried(p-side,deltaPx,dims));
    vec4 k=mix(reservoir,u,e);
    vec4 q=(1.0-clamp(dilution,0.0,1.0))*mix(freshInk,k,clamp(blending,0.0,1.0));
    if(watercolor && emitSource) { pixel=w*q; return; }
    pixel=watercolor ? w*q+(1.0-w*q.a)*src : mix(src,q,w);
}
)glsl";

// §9.2 — Gaussian blur, one direction, unchanged.
inline constexpr const char* kGaussianFragment = R"glsl(#version 330 core
uniform sampler2D imageIn;
uniform ivec2 direction;
uniform int radiusPx;
layout(location=0) out vec4 pixel;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy), hi=textureSize(imageIn,0)-1;
    int r=clamp(radiusPx,0,32);
    if(r==0) { pixel=texelFetch(imageIn,p,0); return; }
    float sigma=float(r)/3.0, weights=0.0;
    vec4 sum=vec4(0);
    for(int i=-r;i<=r;++i) {
        float w=exp(-float(i*i)/(2.0*sigma*sigma));
        sum+=w*texelFetch(imageIn,clamp(p+i*direction,ivec2(0),hi),0);
        weights+=w;
    }
    pixel=sum/weights;
}
)glsl";

// §5: the surface under the stamp centre, C = [4·S(c) + S(c ± (r/2, 0)) + S(c ± (0, r/2))] / 8, bilinear,
// into a 1 × 1 RGBA32F target that the CPU reads to update the reservoir H′ once per stamp.
inline constexpr const char* kReservoirFragment = R"glsl(#version 330 core
uniform sampler2D imageIn;
uniform vec2 centerPx;
uniform float halfRadius;
layout(location=0) out vec4 pixel;
vec4 S(vec2 p) { return textureLod(imageIn,p/vec2(textureSize(imageIn,0)),0.0); }
void main() {
    pixel=(4.0*S(centerPx)+S(centerPx+vec2(halfRadius,0))+S(centerPx-vec2(halfRadius,0))+
           S(centerPx+vec2(0,halfRadius))+S(centerPx-vec2(0,halfRadius)))/8.0;
}
)glsl";

// §5 Blur Brush: P′ = mix(S, Pblur, m·o·flow·strength).
inline constexpr const char* kBlurMixFragment = R"glsl(#version 330 core
uniform sampler2D imageIn, blurred, brushMask;
uniform float strength;
layout(location=0) out vec4 pixel;
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
    float w=clamp(texelFetch(brushMask,at,0).r*strength,0.0,1.0);
    pixel=mix(texelFetch(imageIn,at,0),texelFetch(blurred,at,0),w);
}
)glsl";

// §7 Alpha Lock for the wet tools: the straight colour of the candidate on the old alpha; where the
// candidate has no alpha, the old pixel. Under the lock the old alpha is the stroke's a0.
inline constexpr const char* kLockApplyFragment = R"glsl(#version 330 core
uniform sampler2D candidate, original;
layout(location=0) out vec4 pixel;
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
    vec4 c=texelFetch(candidate,at,0), o=texelFetch(original,at,0);
    pixel=c.a>0.0 ? vec4(clamp(c.rgb/c.a,0.0,1.0)*o.a,o.a) : o;
}
)glsl";

// Selection boundary (§10.1) in screen space, 1 px wide at any zoom: a pixel of the tile whose document
// position is inside (mask ≥ ½) and whose neighbour one screen pixel away is outside. Neighbouring
// mask tiles are bound as L, R, U, D (zero textures where there are none). Black and white dashes.
inline constexpr const char* kSelectionFragment = R"glsl(#version 330 core
uniform sampler2D maskC, maskL, maskR, maskU, maskD;
uniform vec2 tileOrigin;  // window px (top-left origin) of the tile's document corner
uniform float scale;      // window px per document px
uniform float screenHeight;
in vec2 uv;
layout(location=0) out vec4 pixel;
float maskAt(vec2 d) {
    ivec2 t=ivec2(floor(d));
    if(t.y<0 || t.y>=128) {
        if(t.x<0 || t.x>=128) return 0.0;
        return t.y<0 ? texelFetch(maskU,ivec2(t.x,t.y+128),0).r : texelFetch(maskD,ivec2(t.x,t.y-128),0).r;
    }
    if(t.x<0) return texelFetch(maskL,ivec2(t.x+128,t.y),0).r;
    if(t.x>=128) return texelFetch(maskR,ivec2(t.x-128,t.y),0).r;
    return texelFetch(maskC,t,0).r;
}
void main() {
    vec2 s=vec2(gl_FragCoord.x,screenHeight-gl_FragCoord.y);
    vec2 d=(s-tileOrigin)/scale;
    if(maskAt(d)<0.5) discard;
    float step=1.0/scale;
    bool edge=maskAt(d+vec2(step,0))<0.5 || maskAt(d-vec2(step,0))<0.5 ||
              maskAt(d+vec2(0,step))<0.5 || maskAt(d-vec2(0,step))<0.5;
    if(!edge) discard;
    float dash=mod(floor((s.x+s.y)/4.0),2.0);
    pixel=vec4(vec3(dash),1.0);
}
)glsl";

// §9.3 — Tone Curve, unchanged.
inline constexpr const char* kToneCurveFragment = R"glsl(#version 330 core
uniform sampler2D imageIn, curveTable;
uniform bool discrete;
layout(location=0) out vec4 pixel;
float curve(float v,int channel) {
    int last=textureSize(curveTable,0).x-1;
    float x=clamp(v,0.0,1.0)*float(last);
    int i=int(floor(x));
    if(discrete) return texelFetch(curveTable,ivec2(int(floor(x+0.5)),0),0)[channel];
    float a=texelFetch(curveTable,ivec2(i,0),0)[channel];
    float b=texelFetch(curveTable,ivec2(min(i+1,last),0),0)[channel];
    return mix(a,b,fract(x));
}
void main() {
    vec4 c=texelFetch(imageIn,ivec2(gl_FragCoord.xy),0);
    if(c.a<=0.0) { pixel=vec4(0); return; }
    vec3 s=c.rgb/c.a;
    pixel=vec4(c.a*vec3(curve(s.r,0),curve(s.g,1),curve(s.b,2)),c.a);
}
)glsl";

// §9.4 — Unsharp Mask, unchanged.
inline constexpr const char* kUnsharpFragment = R"glsl(#version 330 core
uniform sampler2D imageIn, blurred;
uniform float gain, threshold;
layout(location=0) out vec4 pixel;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    vec4 c=texelFetch(imageIn,p,0), b=texelFetch(blurred,p,0);
    if(c.a<=0.0 || b.a<=0.0) { pixel=c; return; }
    vec3 s=c.rgb/c.a, d=s-b.rgb/b.a, v=abs(d);
    if(max(max(v.r,v.g),v.b)<clamp(threshold,0.0,1.0)) { pixel=c; return; }
    pixel=vec4(c.a*clamp(s+clamp(gain,0.0,4.0)*d,0.0,1.0),c.a);
}
)glsl";

// §9.5 — Chromatic Aberration, unchanged.
inline constexpr const char* kChromaticFragment = R"glsl(#version 330 core
uniform sampler2D imageIn;
uniform vec2 shiftPx;
layout(location=0) out vec4 pixel;
vec4 readLinear(vec2 p) {
    ivec2 hi=textureSize(imageIn,0)-1;
    p=clamp(p,vec2(0),vec2(hi));
    ivec2 a=ivec2(floor(p)), b=min(a+1,hi);
    vec2 f=fract(p);
    vec4 top=mix(texelFetch(imageIn,a,0),texelFetch(imageIn,ivec2(b.x,a.y),0),f.x);
    vec4 bottom=mix(texelFetch(imageIn,ivec2(a.x,b.y),0),texelFetch(imageIn,b,0),f.x);
    return mix(top,bottom,f.y);
}
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    vec4 c=texelFetch(imageIn,p,0);
    if(c.a<=0.0) { pixel=vec4(0); return; }
    vec2 d=clamp(shiftPx,vec2(-32),vec2(32));
    vec4 r=readLinear(vec2(p)+d), b=readLinear(vec2(p)-d);
    float red=r.a>0.0?r.r/r.a:c.r/c.a;
    float blue=b.a>0.0?b.b/b.a:c.b/c.a;
    pixel=vec4(c.a*clamp(red,0.0,1.0),c.g,c.a*clamp(blue,0.0,1.0),c.a);
}
)glsl";

// §9.6 — Anime Line Extraction, unchanged.
inline constexpr const char* kAnimeLinesFragment = R"glsl(#version 330 core
uniform sampler2D imageIn;
uniform float blackPoint, whitePoint, mid, edgeGain;
layout(location=0) out vec4 pixel;
float gray(ivec2 p) {
    ivec2 hi=textureSize(imageIn,0)-1;
    vec4 c=texelFetch(imageIn,clamp(p,ivec2(0),hi),0);
    return clamp(dot(c.rgb,vec3(.298912,.586611,.114478))+1.0-c.a,0.0,1.0);
}
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    float lo=clamp(blackPoint,0.0,1.0-1.0/255.0);
    float hi=clamp(whitePoint,lo+1.0/255.0,1.0);
    float m=clamp(mid,.01,.99), t=clamp((gray(p)-lo)/(hi-lo),0.0,1.0);
    float y=t<=m ? t*(1.0-m)/m : 1.0-(1.0-t)*m/(1.0-m);
    float nw=gray(p+ivec2(-1,-1)), n=gray(p+ivec2(0,-1)), ne=gray(p+ivec2(1,-1));
    float w=gray(p+ivec2(-1,0)), e=gray(p+ivec2(1,0));
    float sw=gray(p+ivec2(-1,1)), s=gray(p+ivec2(0,1)), se=gray(p+ivec2(1,1));
    vec2 grad=vec2(ne+2.0*e+se-nw-2.0*w-sw,sw+2.0*s+se-nw-2.0*n-ne);
    float edge=clamp(length(grad)*0.25*clamp(edgeGain,0.0,4.0),0.0,1.0);
    pixel=vec4(0,0,0,max(1.0-y,edge));
}
)glsl";

// §9: the common last step out = mix(original, filtered, selection·amount), once after all passes; for
// "to a new layer" the filtered result alone, weighted the same way.
inline constexpr const char* kFxMixFragment = R"glsl(#version 330 core
uniform sampler2D original, filtered, selection;
uniform float amount;
uniform bool toNewLayer;
layout(location=0) out vec4 pixel;
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
    float k=clamp(texelFetch(selection,at,0).r*amount,0.0,1.0);
    vec4 f=texelFetch(filtered,at,0);
    pixel=toNewLayer ? f*k : mix(texelFetch(original,at,0),f,k);
}
)glsl";

// Brush outline: a dark 1-px ring at the physical radius with a light ring just outside, readable on
// any colour. Blended with (SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
inline constexpr const char* kRingFragment = R"glsl(#version 330 core
uniform vec2 center;
uniform float radius;
layout(location=0) out vec4 pixel;
float ring(float d,float r) { return clamp(1.0-abs(d-r),0.0,1.0); }
void main() {
    float d=distance(gl_FragCoord.xy,center);
    float dark=ring(d,radius), light=ring(d,radius+1.0);
    float a=max(dark,light);
    if(a<=0.0) discard;
    pixel=vec4(dark>=light ? vec3(0.0) : vec3(1.0),0.85*a);
}
)glsl";

}  // namespace chroma::gl::shaders
