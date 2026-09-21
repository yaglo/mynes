#include "gpu_output.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <math.h>

void gpu_output_geometry(SDL_Window *window, GPUOutputGeometry *out) {
    *out=(GPUOutputGeometry){.scale_x=1,.scale_y=1};
    NSWindow *ns=(__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,NULL);
    NSScreen *screen=ns.screen;
    if (!screen) return;
    CGDirectDisplayID display=[screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    /* Enumerating modes is only necessary when moving to another display. */
    static CGDirectDisplayID cached_display;
    static int native_w,native_h;
    if (display!=cached_display) {
        native_w=native_h=0; cached_display=display;
        CFArrayRef modes=CGDisplayCopyAllDisplayModes(display,NULL);
        if(modes) {
            for(CFIndex i=0;i<CFArrayGetCount(modes);i++) {
                CGDisplayModeRef mode=(CGDisplayModeRef)CFArrayGetValueAtIndex(modes,i);
                if(CGDisplayModeGetIOFlags(mode)&kDisplayModeNativeFlag) {
                    int w=(int)CGDisplayModeGetPixelWidth(mode),h=(int)CGDisplayModeGetPixelHeight(mode);
                    if(w*h>native_w*native_h) { native_w=w;native_h=h; }
                }
            }
            CFRelease(modes);
        }
    }
    if(native_w<=0 || native_h<=0) return;
    NSRect rect=[ns.contentView convertRect:ns.contentView.bounds toView:nil];
    rect=[ns convertRectToScreen:rect];
    int drawable_w,drawable_h;
    if(!SDL_GetWindowSizeInPixels(window,&drawable_w,&drawable_h) || drawable_w<=0 || drawable_h<=0) return;
    float xscale=native_w/screen.frame.size.width,yscale=native_h/screen.frame.size.height;
    out->scale_x=rect.size.width*xscale/drawable_w;
    out->scale_y=rect.size.height*yscale/drawable_h;
    out->origin_x=(NSMinX(rect)-NSMinX(screen.frame))*xscale;
    out->origin_y=(NSMaxY(screen.frame)-NSMaxY(rect))*yscale;
    out->native_w=native_w;out->native_h=native_h;out->native_known=true;
    out->resampled=fabsf(out->scale_x-1)>.001f || fabsf(out->scale_y-1)>.001f;
}
