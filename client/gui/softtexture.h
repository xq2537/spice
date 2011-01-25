
#ifndef _softtexture_h_
#define _softtexture_h_

#include <stdint.h>
/* CEGUI 0.6 bug, CEGUITexture.h doesn't include this, we need to */
#include <cstddef>

#include "CEGUIBase.h"
#include "CEGUITexture.h"

namespace CEGUI
{
    class SoftTexture : public Texture
    {
    public:
        SoftTexture(Renderer* owner);
        SoftTexture(Renderer* owner, uint size);
        SoftTexture(Renderer* owner, const String& filename,
                    const String& resourceGroup);
        virtual ~SoftTexture();

        virtual ushort getWidth(void) const { return _width;}
        virtual ushort getHeight(void) const { return _height;}

        virtual void loadFromFile(const String& filename, const String& resourceGroup);
        virtual void loadFromMemory(const void* buffPtr, uint buffWidth, uint buffHeight,
                                    PixelFormat pixelFormat);

    private:
        void freeSurf();

    private:
        uint32_t* _surf;
        ushort _width;
        ushort _height;

        friend class SoftRenderer;
    };
}

#endif

