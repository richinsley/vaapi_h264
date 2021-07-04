package vaapi_h264

import (
	"github.com/pion/mediadevices/pkg/codec"
	"github.com/pion/mediadevices/pkg/io/video"
	"github.com/pion/mediadevices/pkg/prop"
)

// Params stores libx264 specific encoding parameters.
type Params struct {
	codec.BaseParams
}

type VAAPI_FOURCC uint

// List of the RateControlMode.
const (
	/** NV12: two-plane 8-bit YUV 4:2:0.
	* The first plane contains Y, the second plane contains U and V in pairs of bytes.
	 */
	VA_FOURCC_NV12 VAAPI_FOURCC = 0x3231564E
	/** NV21: two-plane 8-bit YUV 4:2:0.
	* Same as NV12, but with U and V swapped.
	 */
	VA_FOURCC_NV21 VAAPI_FOURCC = 0x3132564E
	/** RGBA: packed 8-bit RGBA.
	*
	* Four bytes per pixel: red, green, blue, alpha.
	 */
	VA_FOURCC_RGBA VAAPI_FOURCC = 0x41424752
	/** RGBX: packed 8-bit RGB.
	*
	* Four bytes per pixel: red, green, blue, unspecified.
	 */
	VA_FOURCC_RGBX VAAPI_FOURCC = 0x58424752
	/** BGRA: packed 8-bit RGBA.
	*
	* Four bytes per pixel: blue, green, red, alpha.
	 */
	VA_FOURCC_BGRA VAAPI_FOURCC = 0x41524742
	/** BGRX: packed 8-bit RGB.
	*
	* Four bytes per pixel: blue, green, red, unspecified.
	 */
	VA_FOURCC_BGRX VAAPI_FOURCC = 0x58524742
	/** ARGB: packed 8-bit RGBA.
	*
	* Four bytes per pixel: alpha, red, green, blue.
	 */
	VA_FOURCC_ARGB VAAPI_FOURCC = 0x42475241
	/** XRGB: packed 8-bit RGB.
	*
	* Four bytes per pixel: unspecified, red, green, blue.
	 */
	VA_FOURCC_XRGB VAAPI_FOURCC = 0x42475258
	/** ABGR: packed 8-bit RGBA.
	*
	* Four bytes per pixel: alpha, blue, green, red.
	 */
	VA_FOURCC_ABGR VAAPI_FOURCC = 0x52474241
	/** XBGR: packed 8-bit RGB.
	*
	* Four bytes per pixel: unspecified, blue, green, red.
	 */
	VA_FOURCC_XBGR VAAPI_FOURCC = 0x52474258
	/** UYUV: packed 8-bit YUV 4:2:2.
	*
	* Four bytes per pair of pixels: U, Y, U, V.
	 */
	VA_FOURCC_UYVY VAAPI_FOURCC = 0x59565955
	/** YUY2: packed 8-bit YUV 4:2:2.
	*
	* Four bytes per pair of pixels: Y, U, Y, V.
	 */
	VA_FOURCC_YUY2 VAAPI_FOURCC = 0x32595559
	/** NV11: two-plane 8-bit YUV 4:1:1.
	*
	* The first plane contains Y, the second plane contains U and V in pairs of bytes.
	 */
	VA_FOURCC_NV11 VAAPI_FOURCC = 0x3131564e
	/** YV12: three-plane 8-bit YUV 4:2:0.
	*
	* The three planes contain Y, V and U respectively.
	 */
	VA_FOURCC_YV12 VAAPI_FOURCC = 0x32315659
	/** P208: two-plane 8-bit YUV 4:2:2.
	*
	* The first plane contains Y, the second plane contains U and V in pairs of bytes.
	 */
	VA_FOURCC_P208 VAAPI_FOURCC = 0x38303250
	/** I420: three-plane 8-bit YUV 4:2:0.
	*
	* The three planes contain Y, U and V respectively.
	 */
	VA_FOURCC_I420 VAAPI_FOURCC = 0x30323449
	/** YV24: three-plane 8-bit YUV 4:4:4.
	*
	* The three planes contain Y, V and U respectively.
	 */
	VA_FOURCC_YV24 VAAPI_FOURCC = 0x34325659
)

// NewParams returns default vaapi h264 codec specific parameters.
func NewParams() (Params, error) {
	return Params{
		BaseParams: codec.BaseParams{
			KeyFrameInterval: 60,
		},
	}, nil
}

// RTPCodec represents the codec metadata
func (p *Params) RTPCodec() *codec.RTPCodec {
	return codec.NewRTPH264Codec(90000)
}

// BuildVideoEncoder builds x264 encoder with given params
func (p *Params) BuildVideoEncoder(r video.Reader, property prop.Media) (codec.ReadCloser, error) {
	return newEncoder(r, property, *p)
}
