// +build dragonfly freebsd linux netbsd openbsd solaris

package vaapi_h264

// reference: https://github.com/intel/libva-utils/blob/master/encode/h264encode.c

// #cgo CFLAGS: -DHAVE_VA_DRM=1
// #cgo pkg-config: libva libva-drm
// #include "va_h264.h"
import "C"
import (
	"errors"
	"image"
	"io"
	"sync"
	"unsafe"

	"github.com/pion/mediadevices/pkg/codec"
	"github.com/pion/mediadevices/pkg/io/video"
	"github.com/pion/mediadevices/pkg/prop"
)

type encoder struct {
	context  unsafe.Pointer
	r        video.Reader
	mu       sync.Mutex
	closed   bool
	forceIDR bool
}

func newEncoder(r video.Reader, p prop.Media, params Params) (codec.ReadCloser, error) {
	if params.KeyFrameInterval <= 0 {
		params.KeyFrameInterval = 60
	}

	/*
		Assume frame sequence is: Frame#0,#1,#2,...,#M,...,#X,... (encoding order)
		1) period between Frame #X and Frame #N = #X - #N
		2) 0 means infinite for intra_period/intra_idr_period, and 0 is invalid for ip_period
		3) intra_idr_period % intra_period (intra_period > 0) and intra_period % ip_period must be 0
		4) intra_period and intra_idr_period take precedence over ip_period
		5) if ip_period > 1, intra_period and intra_idr_period are not  the strict periods
			of I/IDR frames, see bellow examples
		-------------------------------------------------------------------
		intra_period intra_idr_period ip_period frame sequence (intra_period/intra_idr_period/ip_period)
		0            ignored          1          IDRPPPPPPP ...     (No IDR/I any more)
		0            ignored        >=2          IDR(PBB)(PBB)...   (No IDR/I any more)
		1            0                ignored    IDRIIIIIII...      (No IDR any more)
		1            1                ignored    IDR IDR IDR IDR...
		1            >=2              ignored    IDRII IDRII IDR... (1/3/ignore)
		>=2          0                1          IDRPPP IPPP I...   (3/0/1)
		>=2          0              >=2          IDR(PBB)(PBB)(IBB) (6/0/3)
													(PBB)(IBB)(PBB)(IBB)...
		>=2          >=2              1          IDRPPPPP IPPPPP IPPPPP (6/18/1)
												IDRPPPPP IPPPPP IPPPPP...
		>=2          >=2              >=2        {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)} (6/18/3)
												{IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)}...
												{IDR(PBB)(PBB)(IBB)(PBB)}           (6/12/3)
												{IDR(PBB)(PBB)(IBB)(PBB)}...
												{IDR(PBB)(PBB)}                     (6/6/3)
												{IDR(PBB)(PBB)}.
	*/

	// when intra_period and intra_idr_period are equal, all intra-frames will be emitted as IDR frames
	context := C.createContext(C.int(p.Width), C.int(p.Height), C.int(params.BitRate), C.int(params.KeyFrameInterval), C.int(params.KeyFrameInterval), C.int(1), C.int(p.FrameRate), C.int(VAProfileH264Main), C.int(RateControlCBR))
	if context == unsafe.Pointer(nil) {
		return nil, errors.New("failed to create vaapi context")
	}

	e := &encoder{
		context:  context,
		r:        video.ToI420(r),
		closed:   false,
		forceIDR: false,
	}
	return e, nil
}

func (e *encoder) Read() ([]byte, func(), error) {
	e.mu.Lock()
	defer e.mu.Unlock()

	if e.closed {
		return nil, func() {}, io.EOF
	}

	img, _, err := e.r.Read()
	if err != nil {
		return nil, func() {}, err
	}
	yuvImg := img.(*image.YCbCr)

	var rc C.int
	s := C.encodeImage(e.context, C.int(VA_FOURCC_I420), (*C.uchar)(&yuvImg.Y[0]), (*C.uchar)(&yuvImg.Cb[0]), (*C.uchar)(&yuvImg.Cr[0]), &rc, C.bool(e.forceIDR))
	e.forceIDR = false
	encoded := C.GoBytes(unsafe.Pointer(s), rc)
	return encoded, func() {}, err
}

func (e *encoder) SetBitRate(b int) error {
	panic("SetBitRate is not implemented")
}

func (e *encoder) ForceKeyFrame() error {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.forceIDR = true
	return nil
}

func (e *encoder) Close() error {
	e.mu.Lock()
	defer e.mu.Unlock()

	if e.closed {
		return nil
	}

	C.destroyContext(e.context)
	e.closed = true
	return nil
}
