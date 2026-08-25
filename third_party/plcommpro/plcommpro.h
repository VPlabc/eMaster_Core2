#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void* PSDK_HANDLE;

PSDK_HANDLE __stdcall Connect(const char* Parameters);
void        __stdcall Disconnect(PSDK_HANDLE handle);
int         __stdcall PullLastError(void);

int __stdcall SetDeviceParam(PSDK_HANDLE handle, const char* ItemValues);
int __stdcall GetDeviceParam(PSDK_HANDLE handle, char* Buffer, int BufferSize, const char* Items);

int __stdcall ControlDevice(PSDK_HANDLE handle, int OperationID, int Param1, int Param2, int Param3, int Param4, const char* Options);

int __stdcall SetDeviceData(PSDK_HANDLE handle, const char* TableName, const char* Data, const char* Options);
int __stdcall GetDeviceData(PSDK_HANDLE handle, char* Buffer, int BufferSize, const char* TableName, const char* FieldNames, const char* Filter, const char* Options);
int __stdcall GetDeviceDataCount(PSDK_HANDLE handle, const char* TableName, const char* Filter, const char* Options);
int __stdcall DeleteDeviceData(PSDK_HANDLE handle, const char* TableName, const char* Data, const char* Options);

int __stdcall GetRTLog(PSDK_HANDLE handle, char* Buffer, int BufferSize);

int __stdcall SearchDevice(char* CommType, char* Address, char* Buffer);
int __stdcall ModifyIPAddress(char* CommType, char* Address, char* Buffer);

int __stdcall SetDeviceFileData(PSDK_HANDLE handle, const char* FileName, char* Buffer, int BufferSize, const char* Options);
int __stdcall GetDeviceFileData(PSDK_HANDLE handle, char* Buffer, int* BufferSize, const char* FileName, const char* Options);

int __stdcall ProcessBackupData(const unsigned char* revBuf, int fileLen, char* outBuf, int outSize);

#ifdef __cplusplus
}
#endif
