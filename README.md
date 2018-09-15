# vendor_qcom_camera
QCOM Camera HAL for Samsung qcom powered devices

This repo holds the semi-working version of a Qcom HAL (this is not working just for development purposes and how the overall Qcom camera system works)
Its primary for the S3Neo but should theoretically work for the msm8226 and msm8974 family.

Current state:
- sets up every parameters from server
- full m_pCapability (querycap) functionality
- camera_open is modified to match samsung standarts
- initdefaultparameters() is modified as samsung doesnt use the parameters from server but hardcode them inside the HALs private Qcamera::Parameters class


To-Do list:
- fix the server calls to kernel (our buffer is too large)
- get the full kernel IOCTL list the server is using including the I2C write arrays
