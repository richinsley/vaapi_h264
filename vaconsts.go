package vaapi_h264

// RateControlMode represents rate control mode.
// Note that supported mode depends on the codec and acceleration hardware.
type RateControlMode uint

// List of the RateControlMode.
const (
	RateControlCBR            RateControlMode = 0x00000002
	RateControlVBR            RateControlMode = 0x00000004
	RateControlVCM            RateControlMode = 0x00000008
	RateControlCQP            RateControlMode = 0x00000010
	RateControlVBRConstrained RateControlMode = 0x00000020
	RateControlICQ            RateControlMode = 0x00000040
	RateControlMB             RateControlMode = 0x00000080
	RateControlCFS            RateControlMode = 0x00000100
	RateControlParallel       RateControlMode = 0x00000200
	RateControlQVBR           RateControlMode = 0x00000400
	RateControlAVBR           RateControlMode = 0x00000800
)

type H264Profile int

const (
	VAProfileH264ConstrainedBaseline	H264Profile = 13
	VAProfileH264Main					H264Profile = 6
	VAProfileH264High					H264Profile = 7
)