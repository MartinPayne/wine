/*
 * IWineD3D implementation
 *
 * Copyright 2002-2004 Jason Edmeades
 * Copyright 2003-2004 Raphael Junqueira
 * Copyright 2004 Christian Costa
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Compile time diagnostics: */

/* Uncomment this to force only a single display mode to be exposed: */
/*#define DEBUG_SINGLE_MODE*/


#include "config.h"
#include "wined3d_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d);
WINE_DECLARE_DEBUG_CHANNEL(d3d_caps);

/**********************************************************
 * Utility functions follow
 **********************************************************/

/* x11drv GDI escapes */
#define X11DRV_ESCAPE 6789
enum x11drv_escape_codes
{
    X11DRV_GET_DISPLAY,   /* get X11 display for a DC */
    X11DRV_GET_DRAWABLE,  /* get current drawable for a DC */
    X11DRV_GET_FONT,      /* get current X font for a DC */
};

/* retrieve the X display to use on a given DC */
inline static Display *get_display( HDC hdc )
{
    Display *display;
    enum x11drv_escape_codes escape = X11DRV_GET_DISPLAY;

    if (!ExtEscape( hdc, X11DRV_ESCAPE, sizeof(escape), (LPCSTR)&escape,
                    sizeof(display), (LPSTR)&display )) display = NULL;
    return display;
}

/**
 * Note: GL seems to trap if GetDeviceCaps is called before any HWND's created
 * ie there is no GL Context - Get a default rendering context to enable the 
 * function query some info from GL                                     
 */    
static WineD3D_Context* WineD3D_CreateFakeGLContext(void) {
    static WineD3D_Context ctx = { NULL, NULL, NULL, 0, 0 };
    WineD3D_Context* ret = NULL;

    if (glXGetCurrentContext() == NULL) {
       BOOL         gotContext  = FALSE;
       BOOL         created     = FALSE;
       XVisualInfo  template;
       HDC          device_context;
       Visual*      visual;
       BOOL         failed = FALSE;
       int          num;
       XWindowAttributes win_attr;
     
       TRACE_(d3d_caps)("Creating Fake GL Context\n");

       ctx.drawable = (Drawable) GetPropA(GetDesktopWindow(), "__wine_x11_whole_window");

       /* Get the display */
       device_context = GetDC(0);
       ctx.display = get_display(device_context);
       ReleaseDC(0, device_context);
     
       /* Get the X visual */
       ENTER_GL();
       if (XGetWindowAttributes(ctx.display, ctx.drawable, &win_attr)) {
           visual = win_attr.visual;
       } else {
           visual = DefaultVisual(ctx.display, DefaultScreen(ctx.display));
       }
       template.visualid = XVisualIDFromVisual(visual);
       ctx.visInfo = XGetVisualInfo(ctx.display, VisualIDMask, &template, &num);
       if (ctx.visInfo == NULL) {
           LEAVE_GL();
           WARN_(d3d_caps)("Error creating visual info for capabilities initialization\n");
           failed = TRUE;
       }
     
       /* Create a GL context */
       if (!failed) {
           ctx.glCtx = glXCreateContext(ctx.display, ctx.visInfo, NULL, GL_TRUE);
       
           if (ctx.glCtx == NULL) {
               LEAVE_GL();
               WARN_(d3d_caps)("Error creating default context for capabilities initialization\n");
               failed = TRUE;
           }
       }
     
       /* Make it the current GL context */
       if (!failed && glXMakeCurrent(ctx.display, ctx.drawable, ctx.glCtx) == False) {
           glXDestroyContext(ctx.display, ctx.glCtx);
           LEAVE_GL();
           WARN_(d3d_caps)("Error setting default context as current for capabilities initialization\n");
           failed = TRUE;        
       }
     
       /* It worked! Wow... */
       if (!failed) {
           gotContext = TRUE;
           created = TRUE;
           ret = &ctx;
       } else {
           ret = NULL;
       }

   } else {
     if (ctx.ref > 0) ret = &ctx;
   }

   if (NULL != ret) InterlockedIncrement(&ret->ref);
   return ret;
}

static void WineD3D_ReleaseFakeGLContext(WineD3D_Context* ctx) {
    /* If we created a dummy context, throw it away */
    if (NULL != ctx) {
        if (0 == InterlockedDecrement(&ctx->ref)) {
            glXMakeCurrent(ctx->display, None, NULL);
            glXDestroyContext(ctx->display, ctx->glCtx);
            ctx->display = NULL;
            ctx->glCtx = NULL;
            LEAVE_GL();
        }
    }
}

static BOOL IWineD3DImpl_FillGLCaps(WineD3D_GL_Info *gl_info, Display* display) {
    const char *GL_Extensions    = NULL;
    const char *GLX_Extensions   = NULL;
    const char *gl_string        = NULL;
    const char *gl_string_cursor = NULL;
    GLint       gl_max;
    Bool        test = 0;
    int         major, minor;

    TRACE_(d3d_caps)("(%p, %p)\n", gl_info, display);

    /* Fill in the GL info retrievable depending on the display */
    if (NULL != display) {
        test = glXQueryVersion(display, &major, &minor);
        gl_info->glx_version = ((major & 0x0000FFFF) << 16) | (minor & 0x0000FFFF);
        gl_string = glXGetClientString(display, GLX_VENDOR);
    } else {
        gl_string = glGetString(GL_VENDOR);
    }
    
    /* Fill in the GL vendor */
    if (strstr(gl_string, "NVIDIA")) {
        gl_info->gl_vendor = VENDOR_NVIDIA;
    } else if (strstr(gl_string, "ATI")) {
        gl_info->gl_vendor = VENDOR_ATI;
    } else {
        gl_info->gl_vendor = VENDOR_WINE;
    }
   
    TRACE_(d3d_caps)("found GL_VENDOR (%s)->(0x%04x)\n", debugstr_a(gl_string), gl_info->gl_vendor);
    
    /* Parse the GL_VERSION field into major and minor information */
    gl_string = glGetString(GL_VERSION);
    switch (gl_info->gl_vendor) {
    case VENDOR_NVIDIA:
        gl_string_cursor = strstr(gl_string, "NVIDIA");
        gl_string_cursor = strstr(gl_string_cursor, " ");
        while (*gl_string_cursor && ' ' == *gl_string_cursor) ++gl_string_cursor;
        if (*gl_string_cursor) {
            char tmp[16];
            int cursor = 0;

            while (*gl_string_cursor <= '9' && *gl_string_cursor >= '0') {
                tmp[cursor++] = *gl_string_cursor;
                ++gl_string_cursor;
            }
            tmp[cursor] = 0;
            major = atoi(tmp);
        
            if (*gl_string_cursor != '.') WARN_(d3d_caps)("malformed GL_VERSION (%s)\n", debugstr_a(gl_string));
            ++gl_string_cursor;

            while (*gl_string_cursor <= '9' && *gl_string_cursor >= '0') {
                tmp[cursor++] = *gl_string_cursor;
                ++gl_string_cursor;
            }
            tmp[cursor] = 0;
            minor = atoi(tmp);
        }
        break;

    case VENDOR_ATI:
        major = minor = 0;
        gl_string_cursor = strchr(gl_string, '-');
        if (gl_string_cursor++) {
            int error = 0;

            /* Check if version number is of the form x.y.z */
            if (*gl_string_cursor > '9' && *gl_string_cursor < '0')
                error = 1;
            if (!error && *(gl_string_cursor+2) > '9' && *(gl_string_cursor+2) < '0')
                error = 1;
            if (!error && *(gl_string_cursor+4) > '9' && *(gl_string_cursor+4) < '0')
                error = 1;
            if (!error && *(gl_string_cursor+1) != '.' && *(gl_string_cursor+3) != '.')
                error = 1;

            /* Mark version number as malformed */
            if (error)
                gl_string_cursor = 0;
        }

        if (!gl_string_cursor)
            WARN_(d3d_caps)("malformed GL_VERSION (%s)\n", debugstr_a(gl_string));
        else {
            major = *gl_string_cursor - '0';
            minor = (*(gl_string_cursor+2) - '0') * 256 + (*(gl_string_cursor+4) - '0');
        }      
        break;

    default:
        major = 0;
        minor = 9;
    }
    gl_info->gl_driver_version = MAKEDWORD_VERSION(major, minor);
    TRACE_(d3d_caps)("found GL_VERSION  (%s)->(0x%08lx)\n", debugstr_a(gl_string), gl_info->gl_driver_version);

    /* Fill in the renderer information */
    gl_string = glGetString(GL_RENDERER);
    strcpy(gl_info->gl_renderer, gl_string);

    switch (gl_info->gl_vendor) {
    case VENDOR_NVIDIA:
        if (strstr(gl_info->gl_renderer, "GeForce4 Ti")) {
            gl_info->gl_card = CARD_NVIDIA_GEFORCE4_TI4600;
        } else if (strstr(gl_info->gl_renderer, "GeForceFX")) {
            gl_info->gl_card = CARD_NVIDIA_GEFORCEFX_5900ULTRA;
        } else {
            gl_info->gl_card = CARD_NVIDIA_GEFORCE4_TI4600;
        }
        break;

    case VENDOR_ATI:
        if (strstr(gl_info->gl_renderer, "RADEON 9800 PRO")) {
            gl_info->gl_card = CARD_ATI_RADEON_9800PRO;
        } else if (strstr(gl_info->gl_renderer, "RADEON 9700 PRO")) {
            gl_info->gl_card = CARD_ATI_RADEON_9700PRO;
        } else {
            gl_info->gl_card = CARD_ATI_RADEON_8500;
        }
        break;

    default:
        gl_info->gl_card = CARD_WINE;
        break;
    }

    TRACE_(d3d_caps)("found GL_RENDERER (%s)->(0x%04x)\n", debugstr_a(gl_info->gl_renderer), gl_info->gl_card);

    /*
     * Initialize openGL extension related variables
     *  with Default values
     */
    memset(&gl_info->supported, 0, sizeof(gl_info->supported));
    gl_info->max_textures   = 1;
    gl_info->ps_arb_version = PS_VERSION_NOT_SUPPORTED;
    gl_info->vs_arb_version = VS_VERSION_NOT_SUPPORTED;
    gl_info->vs_nv_version  = VS_VERSION_NOT_SUPPORTED;
    gl_info->vs_ati_version = VS_VERSION_NOT_SUPPORTED;

    /* Now work out what GL support this card really has */
#define USE_GL_FUNC(type, pfn) gl_info->pfn = NULL;
    GL_EXT_FUNCS_GEN;
#undef USE_GL_FUNC

    /* Retrieve opengl defaults */
    glGetIntegerv(GL_MAX_CLIP_PLANES, &gl_max);
    gl_info->max_clipplanes = min(D3DMAXUSERCLIPPLANES, gl_max);
    TRACE_(d3d_caps)("ClipPlanes support - num Planes=%d\n", gl_max);

    glGetIntegerv(GL_MAX_LIGHTS, &gl_max);
    gl_info->max_lights = gl_max;
    TRACE_(d3d_caps)("Lights support - max lights=%d\n", gl_max);

    /* Parse the gl supported features, in theory enabling parts of our code appropriately */
    GL_Extensions = glGetString(GL_EXTENSIONS);
    TRACE_(d3d_caps)("GL_Extensions reported:\n");  
    
    if (NULL == GL_Extensions) {
        ERR("   GL_Extensions returns NULL\n");      
    } else {
        while (*GL_Extensions != 0x00) {
            const char *Start = GL_Extensions;
            char        ThisExtn[256];

            memset(ThisExtn, 0x00, sizeof(ThisExtn));
            while (*GL_Extensions != ' ' && *GL_Extensions != 0x00) {
                GL_Extensions++;
            }
            memcpy(ThisExtn, Start, (GL_Extensions - Start));
            TRACE_(d3d_caps)("- %s\n", ThisExtn);

            /**
             * ARB 
             */
            if (strcmp(ThisExtn, "GL_ARB_fragment_program") == 0) {
                gl_info->ps_arb_version = PS_VERSION_11;
                TRACE_(d3d_caps)(" FOUND: ARB Pixel Shader support - version=%02x\n", gl_info->ps_arb_version);
                gl_info->supported[ARB_FRAGMENT_PROGRAM] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ARB_multisample") == 0) {
                TRACE_(d3d_caps)(" FOUND: ARB Multisample support\n");
                gl_info->supported[ARB_MULTISAMPLE] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ARB_multitexture") == 0) {
                glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &gl_max);
                TRACE_(d3d_caps)(" FOUND: ARB Multitexture support - GL_MAX_TEXTURE_UNITS_ARB=%u\n", gl_max);
                gl_info->supported[ARB_MULTITEXTURE] = TRUE;
                gl_info->max_textures = min(8, gl_max);
            } else if (strcmp(ThisExtn, "GL_ARB_texture_cube_map") == 0) {
                TRACE_(d3d_caps)(" FOUND: ARB Texture Cube Map support\n");
                gl_info->supported[ARB_TEXTURE_CUBE_MAP] = TRUE;
                TRACE_(d3d_caps)(" IMPLIED: NVIDIA (NV) Texture Gen Reflection support\n");
                gl_info->supported[NV_TEXGEN_REFLECTION] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ARB_texture_compression") == 0) {
                TRACE_(d3d_caps)(" FOUND: ARB Texture Compression support\n");
                gl_info->supported[ARB_TEXTURE_COMPRESSION] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ARB_texture_env_add") == 0) {
                TRACE_(d3d_caps)(" FOUND: ARB Texture Env Add support\n");
                gl_info->supported[ARB_TEXTURE_ENV_ADD] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ARB_texture_env_combine") == 0) {
                TRACE_(d3d_caps)(" FOUND: ARB Texture Env combine support\n");
                gl_info->supported[ARB_TEXTURE_ENV_COMBINE] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ARB_texture_env_dot3") == 0) {
                TRACE_(d3d_caps)(" FOUND: ARB Dot3 support\n");
                gl_info->supported[ARB_TEXTURE_ENV_DOT3] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ARB_texture_border_clamp") == 0) {
                TRACE_(d3d_caps)(" FOUND: ARB Texture border clamp support\n");
                gl_info->supported[ARB_TEXTURE_BORDER_CLAMP] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ARB_texture_mirrored_repeat") == 0) {
                TRACE_(d3d_caps)(" FOUND: ARB Texture mirrored repeat support\n");
                gl_info->supported[ARB_TEXTURE_MIRRORED_REPEAT] = TRUE;
            } else if (strstr(ThisExtn, "GL_ARB_vertex_program")) {
                gl_info->vs_arb_version = VS_VERSION_11;
                TRACE_(d3d_caps)(" FOUND: ARB Vertex Shader support - version=%02x\n", gl_info->vs_arb_version);
                gl_info->supported[ARB_VERTEX_PROGRAM] = TRUE;

            /**
             * EXT
             */
            } else if (strcmp(ThisExtn, "GL_EXT_fog_coord") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Fog coord support\n");
                gl_info->supported[EXT_FOG_COORD] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_paletted_texture") == 0) { /* handle paletted texture extensions */
                TRACE_(d3d_caps)(" FOUND: EXT Paletted texture support\n");
                gl_info->supported[EXT_PALETTED_TEXTURE] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_point_parameters") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Point parameters support\n");
                gl_info->supported[EXT_POINT_PARAMETERS] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_secondary_color") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Secondary coord support\n");
                gl_info->supported[EXT_SECONDARY_COLOR] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_stencil_wrap") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Stencil wrap support\n");
                gl_info->supported[EXT_STENCIL_WRAP] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_texture_compression_s3tc") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Texture S3TC compression support\n");
                gl_info->supported[EXT_TEXTURE_COMPRESSION_S3TC] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_texture_env_add") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Texture Env Add support\n");
                gl_info->supported[EXT_TEXTURE_ENV_ADD] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_texture_env_combine") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Texture Env combine support\n");
                gl_info->supported[EXT_TEXTURE_ENV_COMBINE] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_texture_env_dot3") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Dot3 support\n");
                gl_info->supported[EXT_TEXTURE_ENV_DOT3] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_texture_filter_anisotropic") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Texture Anisotropic filter support\n");
                gl_info->supported[EXT_TEXTURE_FILTER_ANISOTROPIC] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_texture_lod") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Texture LOD support\n");
                gl_info->supported[EXT_TEXTURE_LOD] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_texture_lod_bias") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Texture LOD bias support\n");
                gl_info->supported[EXT_TEXTURE_LOD_BIAS] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_vertex_weighting") == 0) {
                TRACE_(d3d_caps)(" FOUND: EXT Vertex weighting support\n");
                gl_info->supported[EXT_VERTEX_WEIGHTING] = TRUE;

            /**
             * NVIDIA 
             */
            } else if (strstr(ThisExtn, "GL_NV_fog_distance")) {
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Fog Distance support\n");
                gl_info->supported[NV_FOG_DISTANCE] = TRUE;
            } else if (strstr(ThisExtn, "GL_NV_fragment_program")) {
                gl_info->ps_nv_version = PS_VERSION_11;
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Pixel Shader support - version=%02x\n", gl_info->ps_nv_version);
            } else if (strcmp(ThisExtn, "GL_NV_register_combiners") == 0) {
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Register combiners (1) support\n");
                gl_info->supported[NV_REGISTER_COMBINERS] = TRUE;
            } else if (strcmp(ThisExtn, "GL_NV_register_combiners2") == 0) {
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Register combiners (2) support\n");
                gl_info->supported[NV_REGISTER_COMBINERS2] = TRUE;
            } else if (strcmp(ThisExtn, "GL_NV_texgen_reflection") == 0) {
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Texture Gen Reflection support\n");
                gl_info->supported[NV_TEXGEN_REFLECTION] = TRUE;
            } else if (strcmp(ThisExtn, "GL_NV_texture_env_combine4") == 0) {
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Texture Env combine (4) support\n");
                gl_info->supported[NV_TEXTURE_ENV_COMBINE4] = TRUE;
            } else if (strcmp(ThisExtn, "GL_NV_texture_shader") == 0) {
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Texture Shader (1) support\n");
                gl_info->supported[NV_TEXTURE_SHADER] = TRUE;
            } else if (strcmp(ThisExtn, "GL_NV_texture_shader2") == 0) {
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Texture Shader (2) support\n");
                gl_info->supported[NV_TEXTURE_SHADER2] = TRUE;
            } else if (strcmp(ThisExtn, "GL_NV_texture_shader3") == 0) {
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Texture Shader (3) support\n");
                gl_info->supported[NV_TEXTURE_SHADER3] = TRUE;
            } else if (strstr(ThisExtn, "GL_NV_vertex_program")) {
                gl_info->vs_nv_version = max(gl_info->vs_nv_version, (0 == strcmp(ThisExtn, "GL_NV_vertex_program1_1")) ? VS_VERSION_11 : VS_VERSION_10);
                gl_info->vs_nv_version = max(gl_info->vs_nv_version, (0 == strcmp(ThisExtn, "GL_NV_vertex_program2"))   ? VS_VERSION_20 : VS_VERSION_10);
                TRACE_(d3d_caps)(" FOUND: NVIDIA (NV) Vertex Shader support - version=%02x\n", gl_info->vs_nv_version);
                gl_info->supported[NV_VERTEX_PROGRAM] = TRUE;

            /**
             * ATI
             */
            /** TODO */
            } else if (strcmp(ThisExtn, "GL_ATI_texture_env_combine3") == 0) {
                TRACE_(d3d_caps)(" FOUND: ATI Texture Env combine (3) support\n");
                gl_info->supported[ATI_TEXTURE_ENV_COMBINE3] = TRUE;
            } else if (strcmp(ThisExtn, "GL_ATI_texture_mirror_once") == 0) {
                TRACE_(d3d_caps)(" FOUND: ATI Texture Mirror Once support\n");
                gl_info->supported[ATI_TEXTURE_MIRROR_ONCE] = TRUE;
            } else if (strcmp(ThisExtn, "GL_EXT_vertex_shader") == 0) {
                gl_info->vs_ati_version = VS_VERSION_11;
                TRACE_(d3d_caps)(" FOUND: ATI (EXT) Vertex Shader support - version=%02x\n", gl_info->vs_ati_version);
                gl_info->supported[EXT_VERTEX_SHADER] = TRUE;
            }


            if (*GL_Extensions == ' ') GL_Extensions++;
        }
    }

#define USE_GL_FUNC(type, pfn) gl_info->pfn = (type) glXGetProcAddressARB(#pfn);
    GL_EXT_FUNCS_GEN;
#undef USE_GL_FUNC

    if (display != NULL) {
        GLX_Extensions = glXQueryExtensionsString(display, DefaultScreen(display));
        TRACE_(d3d_caps)("GLX_Extensions reported:\n");  
    
        if (NULL == GLX_Extensions) {
            ERR("   GLX_Extensions returns NULL\n");      
        } else {
            while (*GLX_Extensions != 0x00) {
                const char *Start = GLX_Extensions;
                char ThisExtn[256];
           
                memset(ThisExtn, 0x00, sizeof(ThisExtn));
                while (*GLX_Extensions != ' ' && *GLX_Extensions != 0x00) {
                    GLX_Extensions++;
                }
                memcpy(ThisExtn, Start, (GLX_Extensions - Start));
                TRACE_(d3d_caps)("- %s\n", ThisExtn);
                if (*GLX_Extensions == ' ') GLX_Extensions++;
            }
        }
    }

#define USE_GL_FUNC(type, pfn) gl_info->pfn = (type) glXGetProcAddressARB(#pfn);
    GLX_EXT_FUNCS_GEN;
#undef USE_GL_FUNC

    /* Only save the values obtained when a display is provided */
    if (display != NULL) {
        return TRUE;
    } else {
        return FALSE;
    }
}

/**********************************************************
 * IWineD3D implementation follows
 **********************************************************/

UINT     WINAPI  IWineD3DImpl_GetAdapterCount (IWineD3D *iface) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;

    /* FIXME: Set to one for now to imply the display */
    TRACE_(d3d_caps)("(%p): Mostly stub, only returns primary display\n", This);
    return 1;
}

HRESULT  WINAPI  IWineD3DImpl_RegisterSoftwareDevice(IWineD3D *iface, void* pInitializeFunction) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    FIXME("(%p)->(%p): stub\n", This, pInitializeFunction);
    return D3D_OK;
}

HMONITOR WINAPI  IWineD3DImpl_GetAdapterMonitor(IWineD3D *iface, UINT Adapter) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    FIXME_(d3d_caps)("(%p)->(Adptr:%d)\n", This, Adapter);
    if (Adapter >= IWineD3DImpl_GetAdapterCount(iface)) {
        return NULL;
    }
    return D3D_OK;
}

/* FIXME: GetAdapterModeCount and EnumAdapterModes currently only returns modes 
     of the same bpp but different resolutions                                  */

/* Note: dx9 supplies a format. Calls from d3d8 supply D3DFMT_UNKNOWN */
UINT     WINAPI  IWineD3DImpl_GetAdapterModeCount(IWineD3D *iface, UINT Adapter, D3DFORMAT Format) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    TRACE_(d3d_caps)("(%p}->(Adapter: %d, Format: %s)\n", This, Adapter, debug_d3dformat(Format));

    if (Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return 0;
    }

    if (Adapter == 0) { /* Display */
        int i = 0;
        int j = 0;
#if !defined( DEBUG_SINGLE_MODE )
        DEVMODEW DevModeW;

        /* Work out the current screen bpp */
        HDC hdc = CreateDCA("DISPLAY", NULL, NULL, NULL);
        int bpp = GetDeviceCaps(hdc, BITSPIXEL);
        DeleteDC(hdc);

        while (EnumDisplaySettingsExW(NULL, j, &DevModeW, 0)) {
            j++;
            switch (Format)
            {
            case D3DFMT_UNKNOWN: 
                   i++; 
                   break;
            case D3DFMT_X8R8G8B8:   
            case D3DFMT_A8R8G8B8:   
                   if (min(DevModeW.dmBitsPerPel, bpp) == 32) i++; 
                   break;
            case D3DFMT_X1R5G5B5:
            case D3DFMT_A1R5G5B5:
            case D3DFMT_R5G6B5:
                   if (min(DevModeW.dmBitsPerPel, bpp) == 16) i++; 
                   break;
            default:
                   /* Skip other modes as they do not match requested format */
                   break;
            }
        }
#else
        i = 1;
        j = 1;
#endif
        TRACE_(d3d_caps)("(%p}->(Adapter: %d) => %d (out of %d)\n", This, Adapter, i, j);
        return i;
    } else {
        FIXME_(d3d_caps)("Adapter not primary display\n");
    }
    return 0;
}

/* Note: dx9 supplies a format. Calls from d3d8 supply D3DFMT_UNKNOWN */
HRESULT WINAPI IWineD3DImpl_EnumAdapterModes(IWineD3D *iface, UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    TRACE_(d3d_caps)("(%p}->(Adapter:%d, mode:%d, pMode:%p, format:%s)\n", This, Adapter, Mode, pMode, debug_d3dformat(Format));

    /* Validate the parameters as much as possible */
    if (NULL == pMode || 
        Adapter >= IWineD3DImpl_GetAdapterCount(iface) ||
        Mode    >= IWineD3DImpl_GetAdapterModeCount(iface, Adapter, Format)) {
        return D3DERR_INVALIDCALL;
    }

    if (Adapter == 0) { /* Display */
#if !defined( DEBUG_SINGLE_MODE )
        DEVMODEW DevModeW;
        int ModeIdx = 0;
            
        /* Work out the current screen bpp */
        HDC hdc = CreateDCA("DISPLAY", NULL, NULL, NULL);
        int bpp = GetDeviceCaps(hdc, BITSPIXEL);
        DeleteDC(hdc);

        /* If we are filtering to a specific format, then need to skip all unrelated
           modes, but if mode is irrelevant, then we can use the index directly      */
        if (Format == D3DFMT_UNKNOWN) 
        {
            ModeIdx = Mode;
        } else {
            int i = 0;
            int j = 0;
            DEVMODEW DevModeWtmp;
            
            
            while ((Mode+1) < i && EnumDisplaySettingsExW(NULL, j, &DevModeWtmp, 0)) {
                j++;
                switch (Format)
                {
                case D3DFMT_UNKNOWN: 
                       i++; 
                       break;
                case D3DFMT_X8R8G8B8:   
                case D3DFMT_A8R8G8B8:   
                       if (min(DevModeWtmp.dmBitsPerPel, bpp) == 32) i++; 
                       break;
                case D3DFMT_X1R5G5B5:
                case D3DFMT_A1R5G5B5:
                case D3DFMT_R5G6B5:
                       if (min(DevModeWtmp.dmBitsPerPel, bpp) == 16) i++; 
                       break;
                default:
                       /* Skip other modes as they do not match requested format */
                       break;
                }
            }
            ModeIdx = j;
        }

        /* Now get the display mode via the calculated index */
        if (EnumDisplaySettingsExW(NULL, ModeIdx, &DevModeW, 0)) 
        {
            pMode->Width        = DevModeW.dmPelsWidth;
            pMode->Height       = DevModeW.dmPelsHeight;
            bpp                 = min(DevModeW.dmBitsPerPel, bpp);
            pMode->RefreshRate  = D3DADAPTER_DEFAULT;
            if (DevModeW.dmFields&DM_DISPLAYFREQUENCY)
            {
                pMode->RefreshRate = DevModeW.dmDisplayFrequency;
            }

            if (Format == D3DFMT_UNKNOWN)
            {
                switch (bpp) {
                case  8: pMode->Format = D3DFMT_R3G3B2;   break;
                case 16: pMode->Format = D3DFMT_R5G6B5;   break;
                case 24: /* pMode->Format = D3DFMT_R5G6B5;   break;*/ /* Make 24bit appear as 32 bit */
                case 32: pMode->Format = D3DFMT_A8R8G8B8; break;
                default: pMode->Format = D3DFMT_UNKNOWN;
                }
            } else {
                pMode->Format = Format;
            }
        }
        else
        {
            TRACE_(d3d_caps)("Requested mode out of range %d\n", Mode);
            return D3DERR_INVALIDCALL;
        }

#else
        /* Return one setting of the format requested */
        if (Mode > 0) return D3DERR_INVALIDCALL;
        pMode->Width        = 800;
        pMode->Height       = 600;
        pMode->RefreshRate  = D3DADAPTER_DEFAULT;
        pMode->Format       = (Format==D3DFMT_UNKNOWN)?D3DFMT_A8R8G8B8:Format;
        bpp = 32;
#endif
        TRACE_(d3d_caps)("W %d H %d rr %d fmt (%x - %s) bpp %u\n", pMode->Width, pMode->Height, 
                 pMode->RefreshRate, pMode->Format, debug_d3dformat(pMode->Format), bpp);

    } else {
        FIXME_(d3d_caps)("Adapter not primary display\n");
    }

    return D3D_OK;
}

HRESULT WINAPI IWineD3DImpl_GetAdapterDisplayMode(IWineD3D *iface, UINT Adapter, D3DDISPLAYMODE* pMode) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    TRACE_(d3d_caps)("(%p}->(Adapter: %d, pMode: %p)\n", This, Adapter, pMode);

    if (NULL == pMode || 
        Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return D3DERR_INVALIDCALL;
    }

    if (Adapter == 0) { /* Display */
        int bpp = 0;
        DEVMODEW DevModeW;

        EnumDisplaySettingsExW(NULL, (DWORD)-1, &DevModeW, 0);
        pMode->Width        = DevModeW.dmPelsWidth;
        pMode->Height       = DevModeW.dmPelsHeight;
        bpp                 = DevModeW.dmBitsPerPel;
        pMode->RefreshRate  = D3DADAPTER_DEFAULT;
        if (DevModeW.dmFields&DM_DISPLAYFREQUENCY)
        {
            pMode->RefreshRate = DevModeW.dmDisplayFrequency;
        }

        switch (bpp) {
        case  8: pMode->Format       = D3DFMT_R3G3B2;   break;
        case 16: pMode->Format       = D3DFMT_R5G6B5;   break;
        case 24: /*pMode->Format       = D3DFMT_R5G6B5;   break;*/ /* Make 24bit appear as 32 bit */
        case 32: pMode->Format       = D3DFMT_A8R8G8B8; break;
        default: pMode->Format       = D3DFMT_UNKNOWN;
        }

    } else {
        FIXME_(d3d_caps)("Adapter not primary display\n");
    }

    TRACE_(d3d_caps)("returning w:%d, h:%d, ref:%d, fmt:%s\n", pMode->Width,
          pMode->Height, pMode->RefreshRate, debug_d3dformat(pMode->Format));
    return D3D_OK;
}

/* Note due to structure differences between dx8 and dx9 D3DADAPTER_IDENTIFIER,
   and fields being inserted in the middle, a new structure is used in place    */
HRESULT WINAPI IWineD3DImpl_GetAdapterIdentifier(IWineD3D *iface, UINT Adapter, DWORD Flags, 
                                                   WINED3DADAPTER_IDENTIFIER* pIdentifier) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;

    TRACE_(d3d_caps)("(%p}->(Adapter: %d, Flags: %lx, pId=%p)\n", This, Adapter, Flags, pIdentifier);

    if (Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return D3DERR_INVALIDCALL;
    }

    if (Adapter == 0) { /* Display - only device supported for now */
        
        BOOL isGLInfoValid = This->isGLInfoValid;

        /* FillGLCaps updates gl_info, but we only want to store and 
           reuse the values once we have a context which is valid. Values from
           a temporary context may differ from the final ones                 */
        if (isGLInfoValid == FALSE) {
        
          /* If we don't know the device settings, go query them now via a 
             fake context                                                   */
          WineD3D_Context* ctx = WineD3D_CreateFakeGLContext();
          if (NULL != ctx) {
              isGLInfoValid = IWineD3DImpl_FillGLCaps(&This->gl_info, ctx->display);
              WineD3D_ReleaseFakeGLContext(ctx);
          }
        }

        /* If it worked, return the information requested */
        if (isGLInfoValid == TRUE) {
          TRACE_(d3d_caps)("device/Vendor Name and Version detection using FillGLCaps\n");
          strcpy(pIdentifier->Driver, "Display");
          strcpy(pIdentifier->Description, "Direct3D HAL");

          /* Note dx8 doesn't supply a DeviceName */
          if (NULL != pIdentifier->DeviceName) strcpy(pIdentifier->DeviceName, "\\\\.\\DISPLAY"); /* FIXME: May depend on desktop? */
          pIdentifier->DriverVersion->u.HighPart = 0xa;
          pIdentifier->DriverVersion->u.LowPart = This->gl_info.gl_driver_version;
          *(pIdentifier->VendorId) = This->gl_info.gl_vendor;
          *(pIdentifier->DeviceId) = This->gl_info.gl_card;
          *(pIdentifier->SubSysId) = 0;
          *(pIdentifier->Revision) = 0;

        } else {

          /* If it failed, return dummy values from an NVidia driver */
          WARN_(d3d_caps)("Cannot get GLCaps for device/Vendor Name and Version detection using FillGLCaps, currently using NVIDIA identifiers\n");
          strcpy(pIdentifier->Driver, "Display");
          strcpy(pIdentifier->Description, "Direct3D HAL");
          if (NULL != pIdentifier->DeviceName) strcpy(pIdentifier->DeviceName, "\\\\.\\DISPLAY"); /* FIXME: May depend on desktop? */
          pIdentifier->DriverVersion->u.HighPart = 0xa;
          pIdentifier->DriverVersion->u.LowPart = MAKEDWORD_VERSION(53, 96); /* last Linux Nvidia drivers */
          *(pIdentifier->VendorId) = VENDOR_NVIDIA;
          *(pIdentifier->DeviceId) = CARD_NVIDIA_GEFORCE4_TI4600;
          *(pIdentifier->SubSysId) = 0;
          *(pIdentifier->Revision) = 0;
        }

        /*FIXME: memcpy(&pIdentifier->DeviceIdentifier, ??, sizeof(??GUID)); */
        if (Flags & D3DENUM_NO_WHQL_LEVEL) {
            *(pIdentifier->WHQLLevel) = 0;
        } else {
            *(pIdentifier->WHQLLevel) = 1;
        }

    } else {
        FIXME_(d3d_caps)("Adapter not primary display\n");
    }

    return D3D_OK;
}

HRESULT WINAPI IWineD3DImpl_CheckDepthStencilMatch(IWineD3D *iface, UINT Adapter, D3DDEVTYPE DeviceType, 
                D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    WARN_(d3d_caps)("(%p)-> (STUB) (Adptr:%d, DevType:(%x,%s), AdptFmt:(%x,%s), RendrTgtFmt:(%x,%s), DepthStencilFmt:(%x,%s))\n", 
           This, Adapter, 
           DeviceType, debug_d3ddevicetype(DeviceType),
           AdapterFormat, debug_d3dformat(AdapterFormat),
           RenderTargetFormat, debug_d3dformat(RenderTargetFormat), 
           DepthStencilFormat, debug_d3dformat(DepthStencilFormat));

    if (Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return D3DERR_INVALIDCALL;
    }

    return D3D_OK;
}

HRESULT WINAPI IWineD3DImpl_CheckDeviceMultiSampleType(IWineD3D *iface,
                UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat,
                BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) {
    
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    TRACE_(d3d_caps)("(%p)-> (STUB) (Adptr:%d, DevType:(%x,%s), SurfFmt:(%x,%s), Win?%d, MultiSamp:%x, pQual:%p)\n", 
          This, 
          Adapter, 
          DeviceType, debug_d3ddevicetype(DeviceType),
          SurfaceFormat, debug_d3dformat(SurfaceFormat),
          Windowed, 
          MultiSampleType,
          pQualityLevels);
  
    if (Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return D3DERR_INVALIDCALL;
    }

    if (pQualityLevels != NULL) {
        FIXME("Quality levels unsupported at present\n");
        *pQualityLevels = 1; /* Guess at a value! */
    }

    if (D3DMULTISAMPLE_NONE == MultiSampleType)
      return D3D_OK;
    return D3DERR_NOTAVAILABLE;
}

HRESULT WINAPI IWineD3DImpl_CheckDeviceType(IWineD3D *iface,
                UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat,
                D3DFORMAT BackBufferFormat, BOOL Windowed) {

    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    TRACE_(d3d_caps)("(%p)-> (STUB) (Adptr:%d, CheckType:(%x,%s), DispFmt:(%x,%s), BackBuf:(%x,%s), Win?%d): stub\n", 
          This, 
          Adapter, 
          CheckType, debug_d3ddevicetype(CheckType),
          DisplayFormat, debug_d3dformat(DisplayFormat),
          BackBufferFormat, debug_d3dformat(BackBufferFormat),
          Windowed);

    if (Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return D3DERR_INVALIDCALL;
    }

    switch (DisplayFormat) {
      /*case D3DFMT_R5G6B5:*/
    case D3DFMT_R3G3B2:
      return D3DERR_NOTAVAILABLE;
    default:
      break;
    }
    return D3D_OK;
}

HRESULT WINAPI IWineD3DImpl_CheckDeviceFormat(IWineD3D *iface,
                UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    TRACE_(d3d_caps)("(%p)-> (STUB) (Adptr:%d, DevType:(%u,%s), AdptFmt:(%u,%s), Use:(%lu,%s), ResTyp:(%x,%s), CheckFmt:(%u,%s)) ", 
          This, 
          Adapter, 
          DeviceType, debug_d3ddevicetype(DeviceType), 
          AdapterFormat, debug_d3dformat(AdapterFormat), 
          Usage, debug_d3dusage(Usage),
          RType, debug_d3dresourcetype(RType), 
          CheckFormat, debug_d3dformat(CheckFormat));

    if (Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return D3DERR_INVALIDCALL;
    }

    if (GL_SUPPORT(EXT_TEXTURE_COMPRESSION_S3TC)) {
        switch (CheckFormat) {
        case D3DFMT_DXT1:
        case D3DFMT_DXT3:
        case D3DFMT_DXT5:
          TRACE_(d3d_caps)("[OK]\n");
          return D3D_OK;
        default:
            break; /* Avoid compiler warnings */
        }
    }

    switch (CheckFormat) {
    /*****
     * check supported using GL_SUPPORT 
     */
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5: 

    /*****
     *  supported 
     */
      /*case D3DFMT_R5G6B5: */
      /*case D3DFMT_X1R5G5B5:*/
      /*case D3DFMT_A1R5G5B5: */
      /*case D3DFMT_A4R4G4B4:*/

    /*****
     * unsupported 
     */

      /* color buffer */
      /*case D3DFMT_X8R8G8B8:*/
    case D3DFMT_A8R3G3B2:

      /* Paletted */
    case D3DFMT_P8:
    case D3DFMT_A8P8:

      /* Luminance */
    case D3DFMT_L8:
    case D3DFMT_A8L8:
    case D3DFMT_A4L4:

      /* Bump */
#if 0
    case D3DFMT_V8U8:
    case D3DFMT_V16U16:
#endif
    case D3DFMT_L6V5U5:
    case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_W11V11U10:

    /****
     * currently hard to support 
     */
    case D3DFMT_UYVY:
    case D3DFMT_YUY2:

      /* Since we do not support these formats right now, don't pretend to. */
      TRACE_(d3d_caps)("[FAILED]\n");
      return D3DERR_NOTAVAILABLE;
    default:
      break;
    }

    TRACE_(d3d_caps)("[OK]\n");
    return D3D_OK;
}

HRESULT  WINAPI  IWineD3DImpl_CheckDeviceFormatConversion(IWineD3D *iface, UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;

    FIXME_(d3d_caps)("(%p)-> (STUB) (Adptr:%d, DevType:(%u,%s), SrcFmt:(%u,%s), TgtFmt:(%u,%s))",
          This, 
          Adapter, 
          DeviceType, debug_d3ddevicetype(DeviceType), 
          SourceFormat, debug_d3dformat(SourceFormat), 
          TargetFormat, debug_d3dformat(TargetFormat));
    return D3D_OK;
}

/* Note: d3d8 passes in a pointer to a D3DCAPS8 structure, which is a true 
      subset of a D3DCAPS9 structure. However, it has to come via a void * 
      as the d3d8 interface cannot import the d3d9 header                  */
HRESULT WINAPI IWineD3DImpl_GetDeviceCaps(IWineD3D *iface, UINT Adapter, D3DDEVTYPE DeviceType, void* pCapsIn) {

    IWineD3DImpl    *This = (IWineD3DImpl *)iface;
    BOOL             gotContext  = FALSE;
    GLint            gl_tex_size = 0;    
    WineD3D_Context *fake_ctx = NULL;
    D3DCAPS9        *pCaps = (D3DCAPS9 *)pCapsIn;

    TRACE_(d3d_caps)("(%p)->(Adptr:%d, DevType: %x, pCaps: %p)\n", This, Adapter, DeviceType, pCaps);

    if (Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return D3DERR_INVALIDCALL;
    }

    /* Note: GL seems to trap if GetDeviceCaps is called before any HWND's created
       ie there is no GL Context - Get a default rendering context to enable the 
       function query some info from GL                                           */    
    if (glXGetCurrentContext() == NULL) {
        fake_ctx = WineD3D_CreateFakeGLContext();
        if (NULL != fake_ctx) gotContext = TRUE;
    } else {
        gotContext = TRUE;
    }

    if (gotContext == FALSE) {

        FIXME_(d3d_caps)("GetDeviceCaps called but no GL Context - Returning dummy values\n");
        gl_tex_size=65535;
        pCaps->MaxTextureBlendStages = 2;
        pCaps->MaxSimultaneousTextures = 2;
        pCaps->MaxUserClipPlanes = 8;
        pCaps->MaxActiveLights = 8;
        pCaps->MaxVertexBlendMatrices = 0;
        pCaps->MaxVertexBlendMatrixIndex = 1;
        pCaps->MaxAnisotropy = 0;
        pCaps->MaxPointSize = 255.0;
    } else {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl_tex_size);
    }

    /* If we don't know the device settings, go query them now */
    if (This->isGLInfoValid == FALSE) {
        BOOL rc = IWineD3DImpl_FillGLCaps(&This->gl_info, NULL);

        /* If we are running off a real context, save the values */
        if ((rc == TRUE) && ((NULL != fake_ctx))) This->isGLInfoValid = TRUE;
    }

    /* ------------------------------------------------
       The following fields apply to both d3d8 and d3d9
       ------------------------------------------------ */
    pCaps->DeviceType = (DeviceType == D3DDEVTYPE_HAL) ? D3DDEVTYPE_HAL : D3DDEVTYPE_REF;  /* Not quite true, but use h/w supported by opengl I suppose */
    pCaps->AdapterOrdinal = Adapter;

    pCaps->Caps = 0;
    pCaps->Caps2 = D3DCAPS2_CANRENDERWINDOWED;
    pCaps->Caps3 = D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    pCaps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE;

    pCaps->CursorCaps = 0;

    pCaps->DevCaps = D3DDEVCAPS_DRAWPRIMTLVERTEX    | 
                     D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                     D3DDEVCAPS_PUREDEVICE;

    pCaps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLCCW               | 
                               D3DPMISCCAPS_CULLCW                | 
                               D3DPMISCCAPS_COLORWRITEENABLE      |
                               D3DPMISCCAPS_CLIPTLVERTS           |
                               D3DPMISCCAPS_CLIPPLANESCALEDPOINTS | 
                               D3DPMISCCAPS_MASKZ; 
                               /*NOT: D3DPMISCCAPS_TSSARGTEMP*/

    pCaps->RasterCaps = D3DPRASTERCAPS_DITHER   | 
                        D3DPRASTERCAPS_PAT      | 
			D3DPRASTERCAPS_WFOG |
			D3DPRASTERCAPS_ZFOG |
                        D3DPRASTERCAPS_FOGVERTEX |
			D3DPRASTERCAPS_FOGTABLE  |
                        D3DPRASTERCAPS_FOGRANGE;

    if (GL_SUPPORT(EXT_TEXTURE_FILTER_ANISOTROPIC)) {
      pCaps->RasterCaps |= D3DPRASTERCAPS_ANISOTROPY;
    }
                        /* FIXME Add:
			   D3DPRASTERCAPS_MIPMAPLODBIAS
			   D3DPRASTERCAPS_ZBIAS
			   D3DPRASTERCAPS_COLORPERSPECTIVE
			   D3DPRASTERCAPS_STRETCHBLTMULTISAMPLE
			   D3DPRASTERCAPS_ANTIALIASEDGES
			   D3DPRASTERCAPS_ZBUFFERLESSHSR
			   D3DPRASTERCAPS_WBUFFER */

    pCaps->ZCmpCaps = D3DPCMPCAPS_ALWAYS       | 
                      D3DPCMPCAPS_EQUAL        | 
                      D3DPCMPCAPS_GREATER      | 
                      D3DPCMPCAPS_GREATEREQUAL |
                      D3DPCMPCAPS_LESS         | 
                      D3DPCMPCAPS_LESSEQUAL    | 
                      D3DPCMPCAPS_NEVER        |
                      D3DPCMPCAPS_NOTEQUAL;

    pCaps->SrcBlendCaps  = 0xFFFFFFFF;   /*FIXME: Tidy up later */
    pCaps->DestBlendCaps = 0xFFFFFFFF;   /*FIXME: Tidy up later */
    pCaps->AlphaCmpCaps  = 0xFFFFFFFF;   /*FIXME: Tidy up later */

    pCaps->ShadeCaps = D3DPSHADECAPS_SPECULARGOURAUDRGB | 
                       D3DPSHADECAPS_COLORGOURAUDRGB;

    pCaps->TextureCaps =  D3DPTEXTURECAPS_ALPHA        | 
                          D3DPTEXTURECAPS_ALPHAPALETTE | 
                          D3DPTEXTURECAPS_POW2         | 
                          D3DPTEXTURECAPS_VOLUMEMAP    | 
                          D3DPTEXTURECAPS_MIPMAP       |
                          D3DPTEXTURECAPS_PROJECTED;

    if (GL_SUPPORT(ARB_TEXTURE_CUBE_MAP)) {
      pCaps->TextureCaps |= D3DPTEXTURECAPS_CUBEMAP      | 
	                    D3DPTEXTURECAPS_MIPCUBEMAP   | 
	                    D3DPTEXTURECAPS_CUBEMAP_POW2;
    }

    pCaps->TextureFilterCaps = D3DPTFILTERCAPS_MAGFLINEAR | 
                               D3DPTFILTERCAPS_MAGFPOINT  | 
                               D3DPTFILTERCAPS_MINFLINEAR | 
                               D3DPTFILTERCAPS_MINFPOINT  |
                               D3DPTFILTERCAPS_MIPFLINEAR | 
                               D3DPTFILTERCAPS_MIPFPOINT;

    pCaps->CubeTextureFilterCaps = 0;
    pCaps->VolumeTextureFilterCaps = 0;

    pCaps->TextureAddressCaps =  D3DPTADDRESSCAPS_BORDER | 
                                 D3DPTADDRESSCAPS_CLAMP  | 
                                 D3DPTADDRESSCAPS_WRAP;

    if (GL_SUPPORT(ARB_TEXTURE_BORDER_CLAMP)) {
      pCaps->TextureAddressCaps |= D3DPTADDRESSCAPS_BORDER;
    }
    if (GL_SUPPORT(ARB_TEXTURE_MIRRORED_REPEAT)) {
      pCaps->TextureAddressCaps |= D3DPTADDRESSCAPS_MIRROR;
    }
    if (GL_SUPPORT(ATI_TEXTURE_MIRROR_ONCE)) {
      pCaps->TextureAddressCaps |= D3DPTADDRESSCAPS_MIRRORONCE;
    }

    pCaps->VolumeTextureAddressCaps = 0;

    pCaps->LineCaps = D3DLINECAPS_TEXTURE | 
                      D3DLINECAPS_ZTEST;
                      /* FIXME: Add 
			 D3DLINECAPS_BLEND
			 D3DLINECAPS_ALPHACMP
			 D3DLINECAPS_FOG */

    pCaps->MaxTextureWidth = gl_tex_size;
    pCaps->MaxTextureHeight = gl_tex_size;

    pCaps->MaxVolumeExtent = 0;

    pCaps->MaxTextureRepeat = 32768;
    pCaps->MaxTextureAspectRatio = 32768;
    pCaps->MaxVertexW = 1.0;

    pCaps->GuardBandLeft = 0;
    pCaps->GuardBandTop = 0;
    pCaps->GuardBandRight = 0;
    pCaps->GuardBandBottom = 0;

    pCaps->ExtentsAdjust = 0;

    pCaps->StencilCaps =  D3DSTENCILCAPS_DECRSAT | 
                          D3DSTENCILCAPS_INCRSAT | 
                          D3DSTENCILCAPS_INVERT  | 
                          D3DSTENCILCAPS_KEEP    | 
                          D3DSTENCILCAPS_REPLACE | 
                          D3DSTENCILCAPS_ZERO;
    if (GL_SUPPORT(EXT_STENCIL_WRAP)) {
      pCaps->StencilCaps |= D3DSTENCILCAPS_DECR    | 
	                    D3DSTENCILCAPS_INCR;
    }

    pCaps->FVFCaps = D3DFVFCAPS_PSIZE | 0x0008; /* 8 texture coords */

    pCaps->TextureOpCaps =  D3DTEXOPCAPS_ADD         | 
                            D3DTEXOPCAPS_ADDSIGNED   | 
                            D3DTEXOPCAPS_ADDSIGNED2X |
                            D3DTEXOPCAPS_MODULATE    | 
                            D3DTEXOPCAPS_MODULATE2X  | 
                            D3DTEXOPCAPS_MODULATE4X  |
                            D3DTEXOPCAPS_SELECTARG1  | 
                            D3DTEXOPCAPS_SELECTARG2  | 
                            D3DTEXOPCAPS_DISABLE;
#if defined(GL_VERSION_1_3)
    pCaps->TextureOpCaps |= D3DTEXOPCAPS_DOTPRODUCT3 | 
                            D3DTEXOPCAPS_SUBTRACT;
#endif
    if (GL_SUPPORT(ARB_TEXTURE_ENV_COMBINE) || 
	GL_SUPPORT(EXT_TEXTURE_ENV_COMBINE) || 
	GL_SUPPORT(NV_TEXTURE_ENV_COMBINE4)) {
      pCaps->TextureOpCaps |= D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
	                      D3DTEXOPCAPS_BLENDTEXTUREALPHA | 
			      D3DTEXOPCAPS_BLENDFACTORALPHA  |
			      D3DTEXOPCAPS_BLENDCURRENTALPHA |
			      D3DTEXOPCAPS_LERP;
    }
    if (GL_SUPPORT(NV_TEXTURE_ENV_COMBINE4)) {
      pCaps->TextureOpCaps |= D3DTEXOPCAPS_ADDSMOOTH | 
			      D3DTEXOPCAPS_MULTIPLYADD |
			      D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
			      D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA |
                              D3DTEXOPCAPS_BLENDTEXTUREALPHAPM;
    }
    
#if 0
    pCaps->TextureOpCaps |= D3DTEXOPCAPS_BUMPENVMAP;
                            /* FIXME: Add 
			      D3DTEXOPCAPS_BUMPENVMAPLUMINANCE 
			      D3DTEXOPCAPS_PREMODULATE */
#endif

    if (gotContext) {
        GLint gl_max;
	GLfloat gl_float;
#if defined(GL_VERSION_1_3)
        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &gl_max);
#else
        glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &gl_max);
#endif
        TRACE_(d3d_caps)("GLCaps: GL_MAX_TEXTURE_UNITS_ARB=%d\n", gl_max);
        pCaps->MaxTextureBlendStages = min(8, gl_max);
        pCaps->MaxSimultaneousTextures = min(8, gl_max);

        glGetIntegerv(GL_MAX_CLIP_PLANES, &gl_max);
        pCaps->MaxUserClipPlanes = min(D3DMAXUSERCLIPPLANES, gl_max);
        TRACE_(d3d_caps)("GLCaps: GL_MAX_CLIP_PLANES=%ld\n", pCaps->MaxUserClipPlanes);

        glGetIntegerv(GL_MAX_LIGHTS, &gl_max);
        pCaps->MaxActiveLights = gl_max;
        TRACE_(d3d_caps)("GLCaps: GL_MAX_LIGHTS=%ld\n", pCaps->MaxActiveLights);

        if (GL_SUPPORT(ARB_VERTEX_BLEND)) {
	   glGetIntegerv(GL_MAX_VERTEX_UNITS_ARB, &gl_max);
	   pCaps->MaxVertexBlendMatrices = gl_max;
	   pCaps->MaxVertexBlendMatrixIndex = 1;
        } else {
           pCaps->MaxVertexBlendMatrices = 0;
           pCaps->MaxVertexBlendMatrixIndex = 1;
        }

        if (GL_SUPPORT(EXT_TEXTURE_FILTER_ANISOTROPIC)) {
          glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl_max);
          pCaps->MaxAnisotropy = gl_max;
        } else {
          pCaps->MaxAnisotropy = 0;
        }

        glGetFloatv(GL_POINT_SIZE_RANGE, &gl_float);
        pCaps->MaxPointSize = gl_float;
    }

    pCaps->VertexProcessingCaps = D3DVTXPCAPS_DIRECTIONALLIGHTS | 
                                  D3DVTXPCAPS_MATERIALSOURCE7   | 
                                  D3DVTXPCAPS_POSITIONALLIGHTS  | 
                                  D3DVTXPCAPS_LOCALVIEWER |
                                  D3DVTXPCAPS_TEXGEN;
                                  /* FIXME: Add 
                                     D3DVTXPCAPS_TWEENING */

    pCaps->MaxPrimitiveCount = 0xFFFFFFFF;
    pCaps->MaxVertexIndex = 0xFFFFFFFF;
    pCaps->MaxStreams = MAX_STREAMS;
    pCaps->MaxStreamStride = 1024;

    if (((vs_mode == VS_HW) && GL_SUPPORT(ARB_VERTEX_PROGRAM)) || (vs_mode == VS_SW) || (DeviceType == D3DDEVTYPE_REF)) {
      pCaps->VertexShaderVersion = D3DVS_VERSION(1,1);
      
      if (This->gl_info.gl_vendor == VENDOR_MESA || 
          This->gl_info.gl_vendor == VENDOR_WINE) {
        pCaps->MaxVertexShaderConst = 95;
      } else {
        pCaps->MaxVertexShaderConst = WINED3D_VSHADER_MAX_CONSTANTS;
      }
    } else {
      pCaps->VertexShaderVersion = 0;
      pCaps->MaxVertexShaderConst = 0;
    }

    if ((ps_mode == PS_HW) && GL_SUPPORT(ARB_FRAGMENT_PROGRAM) && (DeviceType != D3DDEVTYPE_REF)) {
        pCaps->PixelShaderVersion = D3DPS_VERSION(1,4);
        pCaps->MaxPixelShaderValue = 1.0;
    } else {
        pCaps->PixelShaderVersion = 0;
        pCaps->MaxPixelShaderValue = 0.0;
    }

    /* ------------------------------------------------
       The following fields apply to d3d9 only
       ------------------------------------------------ */
    if (This->dxVersion > 8) {
        FIXME("Caps support for directx9 is non-existant at the moment!\n");
        pCaps->DevCaps2 = 0;
        pCaps->MaxNpatchTessellationLevel = 0;
        pCaps->MasterAdapterOrdinal = 0;
        pCaps->AdapterOrdinalInGroup = 0;
        pCaps->NumberOfAdaptersInGroup = 1;
        pCaps->DeclTypes = 0;
        pCaps->NumSimultaneousRTs = 0;
        pCaps->StretchRectFilterCaps = 0;
        pCaps->VS20Caps.Caps = 0;
        pCaps->PS20Caps.Caps = 0;
        pCaps->VertexTextureFilterCaps = 0;
        pCaps->MaxVShaderInstructionsExecuted = 0;
        pCaps->MaxPShaderInstructionsExecuted = 0;
        pCaps->MaxVertexShader30InstructionSlots = 0;
        pCaps->MaxPixelShader30InstructionSlots = 0;
    }

    /* If we created a dummy context, throw it away */
    if (NULL != fake_ctx) WineD3D_ReleaseFakeGLContext(fake_ctx);
    return D3D_OK;
}

/* Note due to structure differences between dx8 and dx9 D3DPRESENT_PARAMETERS,
   and fields being inserted in the middle, a new structure is used in place    */
HRESULT  WINAPI  IWineD3DImpl_CreateDevice(IWineD3D *iface, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                           DWORD BehaviourFlags, WINED3DPRESENT_PARAMETERS* pPresentationParameters,
                                                            IWineD3DDevice** ppReturnedDeviceInterface) {

    HWND                whichHWND;
    HDC                 hDc;
    IWineD3DDeviceImpl *object  = NULL;
    IWineD3DImpl       *This    = (IWineD3DImpl *)iface;
    int                 num;
    XVisualInfo         template;

    /* Validate the adapter number */
    if (Adapter >= IWineD3D_GetAdapterCount(iface)) {
        return D3DERR_INVALIDCALL;
    }

    /* Create a WineD3DDevice object */
    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(IWineD3DDeviceImpl));
    *ppReturnedDeviceInterface = (IWineD3DDevice *)object;
    TRACE("Created WineD3DDevice object @ %p \n", object);
    if (NULL == object) {
      return D3DERR_OUTOFVIDEOMEMORY;
    }

    /* Set up initial COM information */
    object->lpVtbl  = &IWineD3DDevice_Vtbl;
    object->ref     = 1;
    object->WineD3D = iface;
    IWineD3D_AddRef(object->WineD3D);
    
    TRACE("(%p)->(Adptr:%d, DevType: %x, FocusHwnd: %p, BehFlags: %lx, PresParms: %p, RetDevInt: %p)\n", This, Adapter, DeviceType,
          hFocusWindow, BehaviourFlags, pPresentationParameters, ppReturnedDeviceInterface);
    TRACE("(%p)->(DepthStencil:(%u,%s), BackBufferFormat:(%u,%s))\n", This, 
          *(pPresentationParameters->AutoDepthStencilFormat), debug_d3dformat(*(pPresentationParameters->AutoDepthStencilFormat)),
          *(pPresentationParameters->BackBufferFormat), debug_d3dformat(*(pPresentationParameters->BackBufferFormat)));

    /* Save the creation parameters */
    object->createParms.AdapterOrdinal = Adapter;
    object->createParms.DeviceType     = DeviceType;
    object->createParms.hFocusWindow   = hFocusWindow;
    object->createParms.BehaviorFlags  = BehaviourFlags;

    /* Initialize other useful values */
    object->presentParms.BackBufferCount = 1; /* Opengl only supports one? */
    object->adapterNo                    = Adapter;
    object->devType                      = DeviceType;

    /* Setup hwnd we are using, plus which display this equates to */
    whichHWND = *(pPresentationParameters->hDeviceWindow);
    if (!whichHWND) {
        whichHWND = hFocusWindow;
    }
    object->win_handle = whichHWND;
    object->win        = (Window)GetPropA( whichHWND, "__wine_x11_client_window" );
    hDc                = GetDC(whichHWND);
    object->display    = get_display(hDc);
    ReleaseDC(whichHWND, hDc);

    /* FIXME: Use for dx8 code eventually too! */
    /* Deliberately no indentation here, as this if will be removed when dx8 support merged in */
    if (This->dxVersion > 8) {   

    /* Create a context based off the properties of the existing visual */
    /*   Note the visual is chosen as the window is created and the glcontext cannot
         use different properties after that point in time. FIXME: How to handle when requested format 
         doesn't match actual visual? Cannot choose one here - code removed as it ONLY works if the one
         it chooses is identical to the one already being used!                                        */
    /* FIXME: Handle stencil appropriately via EnableAutoDepthStencil / AutoDepthStencilFormat */
    ENTER_GL();
    template.visualid = (VisualID)GetPropA(GetDesktopWindow(), "__wine_x11_visual_id");
    object->visInfo   = XGetVisualInfo(object->display, VisualIDMask, &template, &num);
    if (NULL == object->visInfo) {
        ERR("cannot really get XVisual\n"); 
        LEAVE_GL();
        return D3DERR_NOTAVAILABLE;
    }
    object->glCtx = glXCreateContext(object->display, object->visInfo, NULL, GL_TRUE);
    if (NULL == object->glCtx) {
        ERR("cannot create glxContext\n"); 
        LEAVE_GL();
        return D3DERR_NOTAVAILABLE;
    }
    LEAVE_GL();

    if (object->glCtx == NULL) {
        ERR("Error in context creation !\n");
        return D3DERR_INVALIDCALL;
    } else {
        TRACE("Context created (HWND=%p, glContext=%p, Window=%ld, VisInfo=%p)\n",
                whichHWND, object->glCtx, object->win, object->visInfo);
    }

    /* If not windowed, need to go fullscreen, and resize the HWND to the appropriate  */
    /*        dimensions                                                               */
    if (!*(pPresentationParameters->Windowed)) {

        DEVMODEW devmode;
        HDC      hdc;
        int      bpp = 0;

        /* Get info on the current display setup */
        hdc = CreateDCA("DISPLAY", NULL, NULL, NULL);
        bpp = GetDeviceCaps(hdc, BITSPIXEL);
        DeleteDC(hdc);

        /* Change the display settings */
        memset(&devmode, 0, sizeof(DEVMODEW));
        devmode.dmFields     = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT; 
        devmode.dmBitsPerPel = (bpp >= 24) ? 32 : bpp;   /*Stupid XVidMode cannot change bpp D3DFmtGetBpp(object, pPresentationParameters->BackBufferFormat);*/
        devmode.dmPelsWidth  = *(pPresentationParameters->BackBufferWidth);
        devmode.dmPelsHeight = *(pPresentationParameters->BackBufferHeight);
        MultiByteToWideChar(CP_ACP, 0, "Gamers CG", -1, devmode.dmDeviceName, CCHDEVICENAME);
        ChangeDisplaySettingsExW(devmode.dmDeviceName, &devmode, object->win_handle, CDS_FULLSCREEN, NULL);

        /* Make popup window */
        SetWindowLongA(whichHWND, GWL_STYLE, WS_POPUP);
        SetWindowPos(object->win_handle, HWND_TOP, 0, 0, 
                     *(pPresentationParameters->BackBufferWidth),
                     *(pPresentationParameters->BackBufferHeight), SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }

    /* MSDN: If Windowed is TRUE and either of the BackBufferWidth/Height values is zero,
       then the corresponding dimension of the client area of the hDeviceWindow
       (or the focus window, if hDeviceWindow is NULL) is taken. */
    if (*(pPresentationParameters->Windowed) && 
        ((*(pPresentationParameters->BackBufferWidth)  == 0) ||
         (*(pPresentationParameters->BackBufferHeight) == 0))) {

        RECT Rect;
        GetClientRect(whichHWND, &Rect);

        if (*(pPresentationParameters->BackBufferWidth) == 0) {
           *(pPresentationParameters->BackBufferWidth) = Rect.right;
           TRACE("Updating width to %d\n", *(pPresentationParameters->BackBufferWidth));
        }
        if (*(pPresentationParameters->BackBufferHeight) == 0) {
           *(pPresentationParameters->BackBufferHeight) = Rect.bottom;
           TRACE("Updating height to %d\n", *(pPresentationParameters->BackBufferHeight));
        }
    }

    /* Save the presentation parms now filled in correctly */
    object->presentParms.BackBufferWidth                = *(pPresentationParameters->BackBufferWidth);
    object->presentParms.BackBufferHeight               = *(pPresentationParameters->BackBufferHeight);           
    object->presentParms.BackBufferFormat               = *(pPresentationParameters->BackBufferFormat);           
    object->presentParms.BackBufferCount                = *(pPresentationParameters->BackBufferCount);            
    object->presentParms.MultiSampleType                = *(pPresentationParameters->MultiSampleType);            
    object->presentParms.MultiSampleQuality             = *(pPresentationParameters->MultiSampleQuality);         
    object->presentParms.SwapEffect                     = *(pPresentationParameters->SwapEffect);                 
    object->presentParms.hDeviceWindow                  = *(pPresentationParameters->hDeviceWindow);              
    object->presentParms.Windowed                       = *(pPresentationParameters->Windowed);                   
    object->presentParms.EnableAutoDepthStencil         = *(pPresentationParameters->EnableAutoDepthStencil);     
    object->presentParms.AutoDepthStencilFormat         = *(pPresentationParameters->AutoDepthStencilFormat);     
    object->presentParms.Flags                          = *(pPresentationParameters->Flags);                      
    object->presentParms.FullScreen_RefreshRateInHz     = *(pPresentationParameters->FullScreen_RefreshRateInHz); 
    object->presentParms.PresentationInterval           = *(pPresentationParameters->PresentationInterval);       

/* TODO:
    * Creating the startup stateBlock *
    object->StateBlock = NULL;
    IWineD3DDevice_CreateStateBlock(object, D3DSBT_ALL, NULL);
    object->UpdateStateBlock = object->StateBlock;
*/    

    /* Setup surfaces for the backbuffer, frontbuffer and depthstencil buffer */
    TRACE("Creating initial device surfaces\n");

/* TODO:
    IWineD3DDevice_CreateRenderTarget((IWineD3DDevice *) object,
                                      *(pPresentationParameters->BackBufferWidth),
                                      *(pPresentationParameters->BackBufferHeight),
                                      *(pPresentationParameters->BackBufferFormat),
                                      *(pPresentationParameters->MultiSampleType),
                                      TRUE,
                                      (IWineD3DSurface *) &object->frontBuffer);

    IWineD3DDevice_CreateRenderTarget((IWineD3DDevice *) object,
                                      *(pPresentationParameters->BackBufferWidth),
                                      *(pPresentationParameters->BackBufferHeight),
                                      *(pPresentationParameters->BackBufferFormat),
                                      *(pPresentationParameters->MultiSampleType),
                                      TRUE,
                                      (IWineD3DSurface *) &object->backBuffer);

    if (*(pPresentationParameters->EnableAutoDepthStencil)) {
       IWineD3DDevice_CreateDepthStencilSurface((IWineD3DDevice *) object,
                                                *(pPresentationParameters->BackBufferWidth),
                                                *(pPresentationParameters->BackBufferHeight),
                                                *(pPresentationParameters->AutoDepthStencilFormat,
                                                D3DMULTISAMPLE_NONE,
                                                (IWineD3DSurface *) &object->depthStencilBuffer);
    } else {
      object->depthStencilBuffer = NULL;
    }
    TRACE("FrontBuf @ %p, BackBuf @ %p, DepthStencil @ %p\n",object->frontBuffer, object->backBuffer, object->depthStencilBuffer);
*/    

    /* init the default renderTarget management */
    object->drawable     = object->win;
    object->render_ctx   = object->glCtx;
/* TODO:
    object->renderTarget = object->backBuffer;
    
    IWineD3DSurface_AddRef((IWineD3DSurface *) object->renderTarget);
    object->stencilBufferTarget = object->depthStencilBuffer;
    if (NULL != object->stencilBufferTarget) {
      IDirect3DSurface8Impl_AddRef((LPDIRECT3DSURFACE8) object->stencilBufferTarget);
    }
*/    

    /* Set up some starting GL setup */
    ENTER_GL();

    if (glXMakeCurrent(object->display, object->win, object->glCtx) == False) {
      ERR("Error in setting current context (context %p drawable %ld)!\n", object->glCtx, object->win);
    }
    checkGLcall("glXMakeCurrent");

    /* Clear the screen */
    glClearColor(1.0, 0.0, 0.0, 0.0);
    checkGLcall("glClearColor");
    glColor3f(1.0, 1.0, 1.0);
    checkGLcall("glColor3f");

    glEnable(GL_LIGHTING);
    checkGLcall("glEnable");

    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);
    checkGLcall("glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);");

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
    checkGLcall("glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);");

    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
    checkGLcall("glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);");

    /* 
     * Initialize openGL extension related variables
     *  with Default values 
     */
    This->isGLInfoValid = IWineD3DImpl_FillGLCaps(&This->gl_info, object->display);

    /* Setup all the devices defaults */
/* TODO: 
    IDirect3DDeviceImpl_InitStartupStateBlock(object); 
*/

    LEAVE_GL();

    { /* Set a default viewport */
       D3DVIEWPORT9 vp;
       vp.X      = 0;
       vp.Y      = 0;
       vp.Width  = *(pPresentationParameters->BackBufferWidth);
       vp.Height = *(pPresentationParameters->BackBufferHeight);
       vp.MinZ   = 0.0f;
       vp.MaxZ   = 1.0f;
       /* TODO: IWineD3DDevice_SetViewport((LPDIRECT3DDEVICE9) object, &vp); */
    }

    /* Initialize the current view state */
    object->modelview_valid = 1;
    object->proj_valid = 0;
    object->view_ident = 1;
    object->last_was_rhw = 0;
    glGetIntegerv(GL_MAX_LIGHTS, &object->maxConcurrentLights);
    TRACE("(%p,%d) All defaults now set up, leaving CreateDevice with %p\n", This, Adapter, object);

    /* Clear the screen */
    /* TODO: IDirect3DDevice8Impl_Clear((LPDIRECT3DDEVICE8) object, 0, NULL, D3DCLEAR_STENCIL|D3DCLEAR_ZBUFFER|D3DCLEAR_TARGET, 0x00, 1.0, 0); */

    } /* End of FIXME: remove when dx8 merged in */

    return D3D_OK;
}

/**********************************************************
 * IUnknown parts follows
 **********************************************************/

HRESULT WINAPI IWineD3DImpl_QueryInterface(IWineD3D *iface,REFIID riid,LPVOID *ppobj)
{
    return E_NOINTERFACE;
}

ULONG WINAPI IWineD3DImpl_AddRef(IWineD3D *iface) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    FIXME("(%p) : AddRef increasing from %ld\n", This, This->ref);
    return InterlockedIncrement(&This->ref);
}

ULONG WINAPI IWineD3DImpl_Release(IWineD3D *iface) {
    IWineD3DImpl *This = (IWineD3DImpl *)iface;
    ULONG ref;
    TRACE("(%p) : Releasing from %ld\n", This, This->ref);
    ref = InterlockedDecrement(&This->ref);
    if (ref == 0) HeapFree(GetProcessHeap(), 0, This);
    return ref;
}

/**********************************************************
 * IWineD3D VTbl follows
 **********************************************************/

IWineD3DVtbl IWineD3D_Vtbl =
{
    IWineD3DImpl_QueryInterface,
    IWineD3DImpl_AddRef,
    IWineD3DImpl_Release,
    IWineD3DImpl_GetAdapterCount,
    IWineD3DImpl_RegisterSoftwareDevice,
    IWineD3DImpl_GetAdapterMonitor,
    IWineD3DImpl_GetAdapterModeCount,
    IWineD3DImpl_EnumAdapterModes,
    IWineD3DImpl_GetAdapterDisplayMode,
    IWineD3DImpl_GetAdapterIdentifier,
    IWineD3DImpl_CheckDeviceMultiSampleType,
    IWineD3DImpl_CheckDepthStencilMatch,
    IWineD3DImpl_CheckDeviceType,
    IWineD3DImpl_CheckDeviceFormat,
    IWineD3DImpl_CheckDeviceFormatConversion,
    IWineD3DImpl_GetDeviceCaps,
    IWineD3DImpl_CreateDevice
};
