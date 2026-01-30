#ifdef _XBOX
#include <xtl.h>

// Global variable to track Xbox memory
static qboolean g_bIsXbox64MB = false;

/*
================
Xbox_DetectMemory

Detect if this is a 64MB or 128MB Xbox console
================
*/
static void Xbox_DetectMemory( void )
{
    MEMORYSTATUS memStatus;
    DWORD totalMem;
    
    GlobalMemoryStatus(&memStatus);
    totalMem = memStatus.dwTotalPhys;
    
    // 64MB systems report around 62-64MB of usable RAM
    // 128MB systems report around 120-128MB of usable RAM
    if (totalMem < (80 * 1024 * 1024))
    {
        g_bIsXbox64MB = true;
        Con_Printf("Xbox Memory: 64MB detected - enabling texture optimizations\n");
    }
    else
    {
        g_bIsXbox64MB = false;
        Con_Printf("Xbox Memory: 128MB detected - using standard texture quality\n");
    }
}

/*
================
Xbox_Is64MB

Returns true if running on 64MB Xbox
================
*/
qboolean Xbox_Is64MB( void )
{
    return g_bIsXbox64MB;
}

#endif // _XBOX
```

**Call `Xbox_DetectMemory()` during initialization** - add this in `R_Init()` or similar:
```c
#ifdef _XBOX
    Xbox_DetectMemory();
#endif
```

---

## PATCH 2: Texture Size Limits (gl_image.c)

**Modify the `GL_SetTextureDimensions` function** (around line 518):

```c
static void GL_SetTextureDimensions( gl_texture_t *tex, int width, int height, int depth )
{
    int	maxTextureSize;
    int	maxDepthSize = 1;

    Assert( tex != NULL );

    switch( tex->target )
    {
    case GL_TEXTURE_1D:
    case GL_TEXTURE_2D:
        maxTextureSize = glConfig.max_2d_texture_size;
        break;
    case GL_TEXTURE_2D_ARRAY_EXT:
        maxDepthSize = glConfig.max_2d_texture_layers;
        maxTextureSize = glConfig.max_2d_texture_size;
        break;
    case GL_TEXTURE_RECTANGLE_EXT:
        maxTextureSize = glConfig.max_2d_rectangle_size;
        break;
    case GL_TEXTURE_CUBE_MAP_ARB:
        maxTextureSize = glConfig.max_cubemap_size;
        break;
    case GL_TEXTURE_3D:
        maxDepthSize = glConfig.max_3d_texture_size;
        maxTextureSize = glConfig.max_3d_texture_size;
        break;
    }

    // *** ADD THIS SECTION ***
    #ifdef _XBOX
    // On 64MB Xbox, aggressively limit texture sizes
    if (Xbox_Is64MB())
    {
        // Limit textures to maximum 256x256 for world/models
        // HUD and UI can stay at 512x512 if they have TF_NOMIPMAP flag
        if (!FBitSet(tex->flags, TF_NOMIPMAP))
        {
            maxTextureSize = Q_min(maxTextureSize, 256);
        }
        else
        {
            // HUD/UI textures - allow up to 512x512
            maxTextureSize = Q_min(maxTextureSize, 512);
        }
    }
    #endif
    // *** END ADDITION ***

    // store original sizes
    tex->srcWidth = width;
    tex->srcHeight = height;

    // ... rest of function continues normally ...
```

---

## PATCH 3: Reduce Mipmap Levels (gl_image.c)

**Modify the `GL_CalcMipmapCount` function** (around line 487):

```c
static int GL_CalcMipmapCount( gl_texture_t *tex, qboolean haveBuffer ) 
{
    int	width, height;
    int	mipcount;

    Assert( tex != NULL );

    if( !haveBuffer || tex->target == GL_TEXTURE_3D )
        return 1;

    // generate mip-levels by user request
    if( FBitSet( tex->flags, TF_NOMIPMAP ))
        return 1;

    // *** ADD THIS SECTION ***
    #ifdef _XBOX
    // On 64MB Xbox, limit mipmaps to save memory
    if (Xbox_Is64MB())
    {
        int maxMips = 4; // Limit to 4 mipmap levels instead of 16
        
        for( mipcount = 0; mipcount < maxMips; mipcount++ )
        {
            width = Q_max( 1, ( tex->width >> mipcount ));
            height = Q_max( 1, ( tex->height >> mipcount ));
            if( width == 1 && height == 1 )
                break;
        }
        
        return mipcount + 1;
    }
    #endif
    // *** END ADDITION ***
        
    // mip-maps can't exceeds 16
    for( mipcount = 0; mipcount < 16; mipcount++ )
    {
        width = Q_max( 1, ( tex->width >> mipcount ));
        height = Q_max( 1, ( tex->height >> mipcount ));
        if( width == 1 && height == 1 )
            break;
    }

    return mipcount + 1;
}
```

---

## PATCH 4: Free Original Texture Data (gl_image.c)

**Modify the `GL_ProcessImage` function** (around line 1285):

```c
static void GL_ProcessImage( gl_texture_t *tex, rgbdata_t *pic )
{
    float	emboss_scale = 0.0f;
    uint	img_flags = 0; 

    // force upload texture as RGB or RGBA (detail textures requires this)
    if( tex->flags & TF_FORCE_COLOR ) pic->flags |= IMAGE_HAS_COLOR;
    if( pic->flags & IMAGE_HAS_ALPHA ) tex->flags |= TF_HAS_ALPHA;

    tex->encode = pic->encode; // share encode method

    if( ImageDXT( pic->type ))
    {
        if( !pic->numMips )
            tex->flags |= TF_NOMIPMAP; // disable mipmapping by user request

        // clear all the unsupported flags
        tex->flags &= ~TF_KEEP_SOURCE;
    }
    else
    {
        // copy flag about luma pixels
        if( pic->flags & IMAGE_HAS_LUMA )
            tex->flags |= TF_HAS_LUMA;

        if( pic->flags & IMAGE_QUAKEPAL )
            tex->flags |= TF_QUAKEPAL;

        // create luma texture from quake texture
        if( tex->flags & TF_MAKELUMA )
        {
            img_flags |= IMAGE_MAKE_LUMA;
            tex->flags &= ~TF_MAKELUMA;
        }

        if( tex->flags & TF_ALLOW_EMBOSS )
        {
            img_flags |= IMAGE_EMBOSS;
            tex->flags &= ~TF_ALLOW_EMBOSS;
        }

        // *** MODIFY THIS SECTION ***
        #ifdef _XBOX
        // On 64MB Xbox, don't keep original texture data to save memory
        if (!FBitSet( tex->flags, TF_IMG_UPLOADED ) && FBitSet( tex->flags, TF_KEEP_SOURCE ))
        {
            if (!Xbox_Is64MB())
            {
                tex->original = FS_CopyImage( pic ); // only keep on 128MB systems
            }
            // On 64MB: don't keep original, we need the memory!
        }
        #else
        if( !FBitSet( tex->flags, TF_IMG_UPLOADED ) && FBitSet( tex->flags, TF_KEEP_SOURCE ))
            tex->original = FS_CopyImage( pic ); // because current pic will be expanded to rgba
        #endif
        // *** END MODIFICATION ***

        // ... rest of function continues ...
```

---

## PATCH 5: Reduce Lightmap Block Size (gl_rsurf.c or gl_local.h)

**Option A: If modifying gl_local.h** (around line 36):

```c
// *** MODIFY THIS ***
#ifdef _XBOX
// Dynamic block size based on Xbox memory
extern int GetOptimalBlockSize( void );
#define BLOCK_SIZE  GetOptimalBlockSize()
#else
#define BLOCK_SIZE  tr.block_size  // lightmap blocksize
#endif

#define BLOCK_SIZE_DEFAULT  128  // for keep backward compatibility
#define BLOCK_SIZE_MAX      1024
```

Then add this function to gl_rsurf.c:

```c
#ifdef _XBOX
/*
================
GetOptimalBlockSize

Returns optimal lightmap block size based on available memory
================
*/
int GetOptimalBlockSize( void )
{
    if (Xbox_Is64MB())
        return 64;  // 64x64 lightmaps on 64MB systems
    else
        return 128; // 128x128 lightmaps on 128MB systems
}
#endif
```

**Option B: Simple modification in R_Init or lightmap initialization**:

```c
#ifdef _XBOX
if (Xbox_Is64MB())
{
    tr.block_size = 64;  // Reduce from 128 to 64 on 64MB systems
    Con_Printf("Lightmap block size: 64 (64MB mode)\n");
}
else
{
    tr.block_size = BLOCK_SIZE_DEFAULT;
    Con_Printf("Lightmap block size: 128 (128MB mode)\n");
}
#endif
```

---

## PATCH 6: Add Forward Declaration (common.h or appropriate header)

Add this declaration so other files can call Xbox_Is64MB():

```c
#ifdef _XBOX
qboolean Xbox_Is64MB( void );
#endif
```

---

## Expected Memory Savings

With these patches applied to a 64MB Xbox:

| Component | Before | After | Savings |
|-----------|--------|-------|---------|
| Texture Resolution | 512x512 | 256x256 | 75% |
| Mipmap Levels | Full chain (~16) | Limited (4) | ~10% |
| Original Data | Kept | Freed | 50% of texture memory |
| Lightmaps | 128x128 blocks | 64x64 blocks | 75% |

**Example calculation for 100 RGBA textures:**
- **Before**: 100 × 512×512×4 bytes × 1.33 (mipmaps) × 2 (keep original) ≈ **280 MB**
- **After**: 100 × 256×256×4 bytes × 1.21 (4 mipmaps) × 1 (no original) ≈ **32 MB**

**Total savings: ~88% reduction in texture memory usage**

---

## Testing Procedure

1. Apply all patches
2. Build for Xbox
3. Test on 64MB console:
   - Verify console message shows "64MB detected"
   - Check textures are visible and not corrupted
   - Verify game loads and runs smoothly
4. Test on 128MB console (if available):
   - Should show "128MB detected"
   - Should use higher quality textures

---

## Additional Optimizations (if still having issues)

If you're still running out of memory, try these additional measures:

### A. Reduce MAX_TEXTURES

In gl_local.h:
```c
#ifdef _XBOX
#define MAX_TEXTURES 2048  // Reduced from 4096 for 64MB Xbox
#else
#define MAX_TEXTURES 4096
#endif
```

### B. Aggressive Texture Unloading

Add a texture cache flush when memory is tight:

```c
#ifdef _XBOX
void R_FlushUnusedTextures( void )
{
    if (!Xbox_Is64MB())
        return;
        
    // Free textures not used in last N frames
    // Implementation would track last-used frame per texture
}
#endif
```

### C. Lower HUD Texture Quality

Even HUD textures can be reduced on 64MB:

```c
#ifdef _XBOX
if (Xbox_Is64MB() && FBitSet(tex->flags, TF_NOMIPMAP))
{
    // HUD textures: reduce from 512 to 256
    maxTextureSize = Q_min(maxTextureSize, 256);
}
#endif
