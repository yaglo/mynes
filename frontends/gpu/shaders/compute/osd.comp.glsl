/* TV-generated RGB overlay, inserted after receiver/matrix decoding and before
 * gun amplifiers. Transparency mixes video drive, not NES palette indices.
 * The 256x240 overlay covers the console picture's place in the decode
 * window; the border around it is left alone. */
#version 450
layout(local_size_x=256) in;
layout(set=0,binding=0) readonly buffer Overlay { uint rgba[]; };
layout(set=1,binding=0) buffer Video { float rgb[]; };
layout(set=2,binding=0) uniform Params { uint count,width; int picture_x,picture_row; uint picture_w; };
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=count) return;
    int px=int(i%width)-picture_x, py=int(i/width)-picture_row;
    if(px<0 || px>=int(picture_w) || py<0 || py>=240) return;
    uint y=uint(py),x=uint(px)*256u/picture_w;
    uint p=rgba[y*256u+x];
    if((p>>24)==0u) return;
    vec4 ui=unpackUnorm4x8(p);
    rgb[3u*i]=mix(rgb[3u*i],ui.r,ui.a);
    rgb[3u*i+1u]=mix(rgb[3u*i+1u],ui.g,ui.a);
    rgb[3u*i+2u]=mix(rgb[3u*i+2u],ui.b,ui.a);
}
