// Portable stub: the engine's HTTP downloader is WinINet-only and unused in
// single-player. Kept behind the same interface so CDownloadManager links.
#include "StdAfx.h"
#include "HTTPDownloader.h"

CHTTPDownloader::CHTTPDownloader() : m_hThread(0), m_hINET(0), m_hUrl(0), m_pBuffer(0), m_iFileSize(0), m_iState(HTTP_STATE_NONE), m_bContinue(false), m_pSystem(0), m_pParent(0) {}
CHTTPDownloader::~CHTTPDownloader() {}
void CHTTPDownloader::InitializeTemplate(IScriptSystem*) {}
int CHTTPDownloader::Create(ISystem* pSystem, CDownloadManager* pParent) { m_pSystem = pSystem; m_pParent = pParent; return 0; }
int CHTTPDownloader::Download(const char*, const char*) { m_iState = HTTP_STATE_ERROR; return 0; }
void CHTTPDownloader::Cancel() { m_iState = HTTP_STATE_CANCELED; }
void CHTTPDownloader::Release() { delete this; }
void CHTTPDownloader::OnError() { m_iState = HTTP_STATE_ERROR; }
void CHTTPDownloader::OnComplete() { m_iState = HTTP_STATE_COMPLETE; }
void CHTTPDownloader::OnCancel() { m_iState = HTTP_STATE_CANCELED; }
int CHTTPDownloader::Download(IFunctionHandler* pH) { return pH->EndFunction(0); }
int CHTTPDownloader::Cancel(IFunctionHandler* pH) { return pH->EndFunction(); }
int CHTTPDownloader::Release(IFunctionHandler* pH) { Release(); return pH->EndFunction(); }
int CHTTPDownloader::GetURL(IFunctionHandler* pH) { return pH->EndFunction(m_szURL.c_str()); }
int CHTTPDownloader::GetFileSize(IFunctionHandler* pH) { return pH->EndFunction(m_iFileSize); }
int CHTTPDownloader::GetFileName(IFunctionHandler* pH) { return pH->EndFunction(m_szDstFile.c_str()); }
DWORD CHTTPDownloader::DownloadProc(CHTTPDownloader*) { return 0; }
void CHTTPDownloader::CreateThread() {}
DWORD CHTTPDownloader::DoDownload() { return 0; }
void CHTTPDownloader::PrepareBuffer() {}
