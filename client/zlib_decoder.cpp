#include "common.h"
#include "zlib_decoder.h"
#include "debug.h"
#include "utils.h"

static void op_decode(SpiceZlibDecoder *decoder,
                      uint8_t *data,
                      int data_size,
                      uint8_t *dest,
                      int dest_size)
{
    ZlibDecoder* _decoder = static_cast<ZlibDecoder*>(decoder);
    _decoder->decode(data, data_size, dest, dest_size);
}

ZlibDecoder::ZlibDecoder()
{
    int z_ret;
    
    _z_strm.zalloc = Z_NULL;
    _z_strm.zfree = Z_NULL;
    _z_strm.opaque = Z_NULL;
    _z_strm.next_in = Z_NULL;
    _z_strm.avail_in = 0;
    z_ret = inflateInit(&_z_strm);
    if (z_ret != Z_OK) {
        THROW("zlib decoder init failed, error %d", z_ret);
    }

    static SpiceZlibDecoderOps decoder_ops = {
        op_decode,
    };

    ops = &decoder_ops;
}

ZlibDecoder::~ZlibDecoder()
{
    inflateEnd(&_z_strm);
}


void ZlibDecoder::decode(uint8_t *data, int data_size, uint8_t *dest, int dest_size)
{
    int z_ret;

    inflateReset(&_z_strm);
    _z_strm.next_in = data;
    _z_strm.avail_in = data_size;
    _z_strm.next_out = dest;
    _z_strm.avail_out = dest_size;

    z_ret = inflate(&_z_strm, Z_FINISH);
   
    if (z_ret != Z_STREAM_END) {
        THROW("zlib inflate failed, error %d", z_ret);
    }
}
