/* NIDL 751810601-120 spatial/tonal surrogate. See docs/fw900-model.md.
 * Gaussian equivalents fit one-on/one-off grille contrast, not a full MTF.
 * Coordinates and integration widths are in the 1920x1200 monitor raster. */
#include "fw900_data.glsl"

float fw900_grid(const float grid[9], vec2 uv) {
    vec2 q = clamp((uv - 0.1) / 0.4, vec2(0), vec2(2));
    ivec2 i = min(ivec2(floor(q)), ivec2(1));
    vec2 f = q - vec2(i);
    int a = i.y*3+i.x;
    return mix(mix(grid[a],grid[a+1],f.x),
               mix(grid[a+3],grid[a+4],f.x),f.y);
}
float fw900_eotf(float voltage) {
    float q = clamp(voltage,0.0,1.0)*255.0;
    int i = min(int(q),254);
    return mix(fw900_tone[i],fw900_tone[i+1],q-float(i));
}
vec3 fw900_pixel(int x, int source_y, bool native_raster) {
    if (source_y < 0 || source_y >= int(source_h)) return vec3(0);
    if (x < 0 || x >= 1920) return vec3(0);
    float sx;
    if (native_raster) sx = float(x);
    else {
        // External scaler: 1600x1200 4:3 picture, 160-pixel side bars.
        if (x < 160 || x >= 1760) return vec3(0);
        sx = (float(x-160)+0.5)*float(signal_w)/1600.0-0.5;
    }
    sx = clamp(sx,0.0,float(signal_w-1u));
    uint x0=uint(sx), x1=min(x0+1u,signal_w-1u);
    uint a=(uint(source_y)*signal_w+x0)*3u;
    uint b=(uint(source_y)*signal_w+x1)*3u;
    vec3 v=mix(vec3(rgb_in[a],rgb_in[a+1u],rgb_in[a+2u]),
               vec3(rgb_in[b],rgb_in[b+1u],rgb_in[b+2u]),fract(sx));
    return vec3(fw900_eotf(v.r),fw900_eotf(v.g),fw900_eotf(v.b));
}
// Antiderivative of Gaussian CDF. Clamp tails to avoid cancellation there.
float fw900_cdf_integral(float x, float sigma) {
    if (x > 5.0*sigma) return x;
    if (x < -5.0*sigma) return 0.0;
    return 0.5*x*(1.0+erf_approx(x*0.70710678118/sigma))
         + sigma*0.3989422804*exp(-0.5*x*x/(sigma*sigma));
}
float fw900_horizontal_weight(float d,float sigma,float width) {
    float hi=d+0.5*width,lo=d-0.5*width;
    return max(0.0,(fw900_cdf_integral(hi+0.5,sigma)
                  -fw900_cdf_integral(hi-0.5,sigma)
                  -fw900_cdf_integral(lo+0.5,sigma)
                  +fw900_cdf_integral(lo-0.5,sigma))/width);
}
vec3 fw900_deposit(vec2 uv) {
    vec2 xy=uv*vec2(1920,1200)-0.5;
    vec2 width=vec2(1920.0/float(out_w),1200.0/float(out_h));
    float sx=fw900_grid(fw900_sigma_x,uv);
    float sy=fw900_grid(fw900_sigma_y,uv);
    int xlo=int(ceil(xy.x-0.5*width.x-4.0*sx-0.5));
    int xhi=int(floor(xy.x+0.5*width.x+4.0*sx+0.5));
    int ylo=max(0,int(ceil(xy.y-0.5*width.y-4.0*sy)));
    int yhi=min(1199,int(floor(xy.y+0.5*width.y+4.0*sy)));
    bool native_raster=source_h==1200u && signal_w==1920u;
    vec3 light=vec3(0);
    // Combine the five identical scaler rows before fetching RGB. Each
    // monitor scanline still deposits its own area-integrated Gaussian.
    int y=ylo;
    while (y<=yhi) {
        int row=native_raster ? y : y/5;
        int end=native_raster ? y : min(yhi,(row+1)*5-1);
        float wy=0.0;
        for (int j=y;j<=end;j++) wy+=beam_coverage(xy.y-float(j),sy,width.y);
        vec3 horizontal=vec3(0);
        for (int x=max(0,xlo);x<=min(1919,xhi);x++)
            horizontal+=fw900_pixel(x,row,native_raster)*fw900_horizontal_weight(xy.x-float(x),sx,width.x);
        light+=horizontal*wy;
        y=end+1;
    }
    return light*fw900_grid(fw900_uniformity,uv);
}

vec3 fw900_render(uint pix) {
    uint d=pix*4u;
    float dwell=max(deflection_x[d+3u],0.0);
    if(dwell<=0.0) return vec3(0);
    vec2 r=vec2(deflection_x[d],deflection_y[d])/vec2(signal_w,out_h);
    vec2 g=vec2(deflection_x[d+1u],deflection_y[d+1u])/vec2(signal_w,out_h);
    vec2 b=vec2(deflection_x[d+2u],deflection_y[d+2u])/vec2(signal_w,out_h);
    vec3 light=fw900_deposit(g);
    // The calibrated preset has coincident guns. Preserve service convergence
    // controls without paying for three deposits in that common case.
    if(any(notEqual(r,g))) light.r=fw900_deposit(r).r;
    if(any(notEqual(b,g))) light.b=fw900_deposit(b).b;
    return light*dwell;
}
