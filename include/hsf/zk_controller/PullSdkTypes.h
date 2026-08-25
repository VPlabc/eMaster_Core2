#pragma once

namespace hsf {

// PullSDK / firmware error codes, ported verbatim from
// ZKTecoProtocol/src/pullsdk/pullsdk_types.h (sdk-protocol-reference.md
// section 5.5 part (1)) -- WinSocket codes (10035, 10054, ...) are a
// separate numeric domain and aren't part of this enum, same as upstream.
enum class PullError : int {
  Ok = 0,
  CommandSendFailed = -1,
  NoResponse = -2,
  BufferTooSmall = -3,
  DecompressFailed = -4,
  InvalidDataLength = -5,
  DecompressedLengthMismatch = -6,
  DuplicateCommand = -7,
  NotAuthorized = -8,
  CrcError = -9,
  DataParseFailed = -10,
  InvalidDataParameter = -11,
  CommandExecutionFailed = -12,
  CommandUnavailable = -13,
  WrongCommPassword = -14,
  FileWriteFailed = -15,
  FileReadFailed = -16,
  FileNotFound = -17,
  UnknownError = -99,
  TableStructureNotFound = -100,
  ConditionFieldNotFound = -101,
  FieldCountMismatch = -102,
  FieldOrderMismatch = -103,
  RealtimeEventDataError = -104,
  DataParseInternalError = -105,
  DataOverflow = -106,
  GetTableStructureFailed = -107,
  InvalidOptions = -108,
  LoadLibraryFailed = -201,
  InterfaceCallFailed = -202,
  CommInitFailed = -203,
  SerialAgentStartFailed = -206,
  WrongTcpIpVersionRequest = -301,
  WrongVersionNumber = -302,
  GetProtocolTypeFailed = -303,
  InvalidSocket = -304,
  SocketError = -305,
  HostError = -306,
  ConnectionFailed = -307,

  // NOT a PullSDK code -- this one is ours, returned by the stub
  // PullSdkClient built when HSF_ENABLE_ZK is OFF (the SDK is 32-bit Windows
  // only). -1000 sits well outside the SDK's own range so it can never
  // collide with a real firmware code, and so "unsupported platform" is
  // never mistaken for a device or network fault someone could act on.
  NotSupported = -1000,
};

// Short human-readable name for zk.lastError() -- doesn't cover every
// WinSocket-domain code (see above), just this enum's own values.
inline const char* PullErrorToString(PullError err) {
  switch (err) {
    case PullError::Ok: return "Ok";
    case PullError::CommandSendFailed: return "CommandSendFailed";
    case PullError::NoResponse: return "NoResponse";
    case PullError::BufferTooSmall: return "BufferTooSmall";
    case PullError::DecompressFailed: return "DecompressFailed";
    case PullError::InvalidDataLength: return "InvalidDataLength";
    case PullError::DecompressedLengthMismatch: return "DecompressedLengthMismatch";
    case PullError::DuplicateCommand: return "DuplicateCommand";
    case PullError::NotAuthorized: return "NotAuthorized";
    case PullError::CrcError: return "CrcError";
    case PullError::DataParseFailed: return "DataParseFailed";
    case PullError::InvalidDataParameter: return "InvalidDataParameter";
    case PullError::CommandExecutionFailed: return "CommandExecutionFailed";
    case PullError::CommandUnavailable: return "CommandUnavailable";
    case PullError::WrongCommPassword: return "WrongCommPassword";
    case PullError::FileWriteFailed: return "FileWriteFailed";
    case PullError::FileReadFailed: return "FileReadFailed";
    case PullError::FileNotFound: return "FileNotFound";
    case PullError::TableStructureNotFound: return "TableStructureNotFound";
    case PullError::ConditionFieldNotFound: return "ConditionFieldNotFound";
    case PullError::FieldCountMismatch: return "FieldCountMismatch";
    case PullError::FieldOrderMismatch: return "FieldOrderMismatch";
    case PullError::RealtimeEventDataError: return "RealtimeEventDataError";
    case PullError::DataParseInternalError: return "DataParseInternalError";
    case PullError::DataOverflow: return "DataOverflow";
    case PullError::GetTableStructureFailed: return "GetTableStructureFailed";
    case PullError::InvalidOptions: return "InvalidOptions";
    case PullError::LoadLibraryFailed: return "LoadLibraryFailed";
    case PullError::InterfaceCallFailed: return "InterfaceCallFailed";
    case PullError::CommInitFailed: return "CommInitFailed";
    case PullError::SerialAgentStartFailed: return "SerialAgentStartFailed";
    case PullError::WrongTcpIpVersionRequest: return "WrongTcpIpVersionRequest";
    case PullError::WrongVersionNumber: return "WrongVersionNumber";
    case PullError::GetProtocolTypeFailed: return "GetProtocolTypeFailed";
    case PullError::InvalidSocket: return "InvalidSocket";
    case PullError::SocketError: return "SocketError";
    case PullError::HostError: return "HostError";
    case PullError::ConnectionFailed: return "ConnectionFailed";
    case PullError::NotSupported: return "NotSupportedOnThisPlatform";
    default: return "UnknownError";
  }
}

}  // namespace hsf
