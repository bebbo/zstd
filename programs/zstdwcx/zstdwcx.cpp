#include <windows.h>
#include <stdlib.h>    // malloc, exit
#include <stdio.h>     // printf
#include <string.h>    // strerror
#include <errno.h>     // errno
#include <sys/stat.h>  // stat
#include <time.h>
#include <share.h>
#include <fcntl.h>
#include <io.h>

#include "zstd.h"
#include "tcmd.h"
#include "resource.h"

#undef MAX_PATH
#define MAX_PATH 1024

static wchar_t inifilename[MAX_PATH];

static wchar_t * qudConvert(wchar_t * dst, char const * src, unsigned int len) {
	wchar_t * s = dst;
	while (--len > 0) {
		wchar_t c = 0xff & *src++;
		if (!c)
			break;
		*dst++ = c;
	}
	*dst = 0;
	return s;
}

static char * qudConvert(char * dst, wchar_t const * src, unsigned int len) {
	char * s = dst;
	if (!src) {
		*s = 0;
		return s;
	}

	while (--len > 0) {
		char c = (char)*src++;
		if (!c)
			break;
		*dst++ = c;
	}
	*dst = 0;
	return s;
}

struct ZHandle {
	int file;
	_int64 cSize;
	_int64 uSize;
	int filetime;
	size_t buffInSize;
	void* buffIn;
	size_t buffOutSize;
	void * buffOut;
	ZSTD_DStream* dstream;
	size_t read, toRead;
	tProcessDataProcW pprocW;
	tProcessDataProc pproc;
	wchar_t arcname[MAX_PATH];
	wchar_t name[MAX_PATH];
	int end;
};

static struct ZHandle defZH;

static int showProgress(struct ZHandle * zh, wchar_t * txt, int delta) {
	if (zh->pprocW)
		return zh->pprocW(txt, delta);

	if (zh->pproc) {
		unsigned char t[MAX_PATH];
		qudConvert((char *)t, txt, MAX_PATH);
		return zh->pproc(t, delta);
	}

	return -1;
}

// ============================================================================
// free a ZHandle
// ============================================================================
static void freeZh(struct ZHandle * zh) {

	if (zh->dstream)
		ZSTD_freeDStream(zh->dstream);

	if (zh->buffIn)
		free(zh->buffIn);

	if (zh->buffOut)
		free(zh->buffOut);

	if (zh->file != -1)
		_close(zh->file);

	delete zh;
}

// ============================================================================
// OpenArchiveW - unicode
// ============================================================================
extern "C" int __stdcall OpenArchiveW(tOpenArchiveDataW* archiveData)
{
	archiveData->OpenResult = E_EOPEN;
	struct ZHandle * zh = new ZHandle();
	do { // while (0);

		struct _stat st;
		if (_wstat(archiveData->ArcNameW, &st))
			break;

		// read file time
		struct tm filetime;
		localtime_s(&filetime, &st.st_mtime);
		unsigned short time = filetime.tm_hour << 11 | filetime.tm_min << 5 | filetime.tm_sec;
		unsigned short date = (filetime.tm_year - 80) << 9 | (filetime.tm_mon + 1) << 5 | filetime.tm_mday;
		zh->filetime = 0x10000 * date + time;

		// open the file
		_wsopen_s(&zh->file, archiveData->ArcNameW, _O_BINARY | _O_RDONLY, _SH_DENYWR, _S_IREAD);
		if (zh->file == -1)
			break;

		// read compressed size
		__int64 size64 = _lseeki64(zh->file, 0, SEEK_END);
		_lseeki64(zh->file, 0, SEEK_SET);

		zh->cSize = st.st_size;

		// copy name and remove the extension
		wcscpy_s(zh->name, MAX_PATH, archiveData->ArcNameW);
		for (int i = wcslen(zh->name); i >= 0; --i) {
			if (zh->name[i] == L'\\') {
				wcscpy_s(zh->name, MAX_PATH, &zh->name[i + 1]);
				break;
			}
		}
		wcscpy_s(zh->arcname, MAX_PATH, zh->name);
		for (int i = wcslen(zh->name); i >= 0; --i) {
			if (zh->name[i] == L'.') {
				zh->name[i] = 0;
				break;
			}
		}

		// init zstandard
		zh->buffInSize = ZSTD_DStreamInSize();
		zh->buffOutSize = ZSTD_DStreamOutSize();
		zh->buffIn = malloc(zh->buffInSize);
		zh->buffOut = malloc(zh->buffOutSize);

		if (!zh->buffIn || !zh->buffOut) {
			archiveData->OpenResult = E_NO_MEMORY;
			break;
		}

		zh->dstream = ZSTD_createDStream();
		zh->toRead = ZSTD_initDStream(zh->dstream);
		if (ZSTD_isError(zh->toRead))
			break;

		// read first block - and more
		int r = _read(zh->file, zh->buffIn, zh->buffInSize);
		if (!r)
			break;

		// rewind to toRead
		zh->read = zh->toRead;
		_lseek(zh->file, zh->toRead, SEEK_SET);

		zh->uSize = ZSTD_getDecompressedSize(zh->buffIn, r);
		if (!zh->uSize)
			zh->uSize = -1;

		archiveData->OpenResult = 0;
		return (int)zh;
	} while (0);

	//cleanup
	freeZh(zh);
	return 0;
}


// ============================================================================
// ProcessFileW
// ============================================================================
extern "C" int __stdcall ProcessFileW(int hArcData, int operation, wchar_t * destPath, wchar_t* destName) {
	if (!hArcData || hArcData == -1)
		return E_BAD_ARCHIVE;

//	if (!destName)
//		return E_NO_FILES;

	struct ZHandle * zh = (struct ZHandle *) hArcData;

	if (operation == PK_SKIP)
		return 0;

	if (!showProgress(zh, destName, 0))
		return E_EABORTED;

	int outfile = -1;
	if (operation == PK_EXTRACT) {
		_wsopen_s(&outfile, destName, _O_BINARY | _O_WRONLY | O_CREAT, _SH_DENYWR, _S_IWRITE);
		if (outfile == -1)
			return E_EWRITE;
	}
	
	int ret = 0;
	while (zh->read) {
		ZSTD_inBuffer input = { zh->buffIn, zh->read, 0 };
		while (input.pos < input.size) {
			ZSTD_outBuffer output = { zh->buffOut, zh->buffOutSize, 0 };
			zh->toRead = ZSTD_decompressStream(zh->dstream, &output, &input);  /* toRead : size of next compressed block */
			if (ZSTD_isError(zh->toRead)) {
				ret = E_BAD_ARCHIVE;
				break;
			}
			if (outfile != -1) {
				int written = _write(outfile, zh->buffOut, output.pos);
				if (written != output.pos) {
					ret = E_EWRITE;
					break;
				}
			}
		}
		if (!showProgress(zh, destName, zh->read))
			return E_EABORTED;
		zh->read = _read(zh->file, zh->buffIn, zh->toRead);
	}

	if (outfile != -1)
		_close(outfile);

	return ret;
}


// ============================================================================
// CloseArchive
// ============================================================================
extern "C" int __stdcall CloseArchive(int hArcData) {
	if (!hArcData || hArcData == -1) return E_ECLOSE;

	freeZh((struct ZHandle *) hArcData);
	return 0;
}

// ============================================================================
// CanYouHandleThisFileW
// ============================================================================
extern "C" int __stdcall CanYouHandleThisFileW(wchar_t* filename) {
	tOpenArchiveDataW oad = {
		filename, 0, 0, filename, 0, 0, 0
	};
	struct ZHandle * zh = (struct ZHandle *)OpenArchiveW(&oad);
	if (!zh)
		return 0;

	freeZh(zh);
	return oad.OpenResult == 0;
}

// ============================================================================
// ReadHeader
// ============================================================================
extern "C" int __stdcall ReadHeader(int hArcData, tHeaderData* headerData) {
	if (!hArcData || hArcData == -1) return E_ABORT;
	struct ZHandle * zh = (struct ZHandle *) hArcData;
	if (zh->end) return E_END_ARCHIVE;
	headerData->PackSize = (int)zh->cSize;
	headerData->UnpSize = (int)zh->uSize;
	headerData->FileTime = zh->filetime;
	qudConvert(headerData->ArcName, zh->arcname, MAX_PATH);
	qudConvert(headerData->FileName, zh->name, MAX_PATH);
	zh->end = 1;
	return 0;
}

// ============================================================================
// ReadHeaderEx(W)
// ============================================================================
extern "C" int __stdcall ReadHeaderExW(int hArcData, tHeaderDataExW* headerData) {
	if (!hArcData || hArcData == -1) return E_ABORT;
	struct ZHandle * zh = (struct ZHandle *) hArcData;
	if (zh->end) return E_END_ARCHIVE;
	headerData->PackSize = (DWORD)zh->cSize;
	headerData->PackSizeHigh = (int)(zh->cSize >> 32);
	headerData->UnpSize = (int)zh->uSize;
	headerData->UnpSizeHigh = (int)(zh->uSize >> 32);
	headerData->FileTime = zh->filetime;

	wcscpy_s(headerData->ArcNameW, MAX_PATH, zh->arcname);
	wcscpy_s(headerData->FileNameW, MAX_PATH, zh->name);
	zh->end = 1;
	return 0;
}

extern "C" int __stdcall ReadHeaderEx(int hArcData, tHeaderDataEx* headerData) {
	if (!hArcData || hArcData == -1) return E_ABORT;
	struct ZHandle * zh = (struct ZHandle *) hArcData;
	if (zh->end) return E_END_ARCHIVE;
	headerData->PackSize = (DWORD) zh->cSize;
	headerData->PackSizeHigh = (int)(zh->cSize >> 32);

	headerData->UnpSize = (int)zh->uSize;
	headerData->UnpSizeHigh = (int)(zh->uSize >> 32);
	headerData->FileTime = zh->filetime;

	qudConvert(headerData->ArcName, zh->arcname, MAX_PATH);
	qudConvert(headerData->FileName, zh->name, MAX_PATH);
	zh->end = 1;
	return 0;
}

// ============================================================================
// PackFilesW
// ============================================================================
extern "C" int __stdcall PackFilesW(wchar_t *packedFile, wchar_t *subPath, wchar_t *srcPath,
	wchar_t *addList, int flags) {
	wchar_t *p = addList;
	// check for one file
	if (!p || !p[0]) return E_NO_FILES;
	while (p[0]) p++;
	p++;
	if (p[0]) return E_TOO_MANY_FILES;

	int level = GetPrivateProfileInt(L"zstdwfx", L"CompressionRate", 3, inifilename);
	if (level <= 0) level = 1;
	if (level >= 19) level = 19;

	wchar_t fullName[MAX_PATH];
	wcscpy_s(fullName, MAX_PATH, srcPath);
	wcsncat_s(fullName, MAX_PATH, addList, MAX_PATH);

	int infile = -1;
	int outfile = -1;
	void* buffIn = 0;
	void* buffOut = 0;
	ZSTD_CStream* cstream = 0;
	int ret = 0;

	do { //while (0);
		_wsopen_s(&infile, fullName, _O_BINARY | _O_RDONLY, _SH_DENYWR, _S_IREAD);
		if (infile == -1) {
			ret = E_EREAD;
			break;
		}

		// do not overwrite existing files.
		_wsopen_s(&outfile, packedFile, _O_BINARY | _O_RDONLY, _SH_DENYWR, _S_IREAD);
		if (outfile != -1) {
			ret = E_ECREATE;
			break;
		}

		_wsopen_s(&outfile, packedFile, _O_BINARY | _O_WRONLY | _O_CREAT, _SH_DENYWR, _S_IWRITE);
		if (outfile == -1) {
			ret = E_ECREATE;
			break;
		}

		// alloc mem
		size_t const buffInSize = ZSTD_CStreamInSize();    /* can always read one full block */
		size_t const buffOutSize = ZSTD_CStreamOutSize();  /* can always flush a full block */
		buffIn = malloc(buffInSize);
		buffOut = malloc(buffOutSize);
		cstream = ZSTD_createCStream();

		if (!buffIn || !buffOut || !cstream) {
			ret = E_NO_MEMORY;
			break;
		}

		size_t const initResult = ZSTD_initCStream(cstream, level);
		if (ZSTD_isError(initResult)) {
			ret = E_NOT_SUPPORTED;
			break;
		}

		if (!showProgress(&defZH, packedFile, 0)) {
			ret = E_EABORTED;
			break;
		}

		size_t read, toRead = buffInSize;
		while (!ret && (read = _read(infile, buffIn, toRead))) {
			ZSTD_inBuffer input = { buffIn, read, 0 };
			while (input.pos < input.size) {
				ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
				toRead = ZSTD_compressStream(cstream, &output, &input);   /* toRead is guaranteed to be <= ZSTD_CStreamInSize() */
				if (ZSTD_isError(toRead)) {
					ret = E_EABORTED;
					break;
				}
				if (toRead > buffInSize) toRead = buffInSize;   /* Safely handle case when `buffInSize` is manually changed to a value < ZSTD_CStreamInSize()*/
				_write(outfile, buffOut, output.pos);
			}

			if (!showProgress(&defZH, packedFile, read))
				ret = E_EABORTED;
		}
		if (!ret) {
			ZSTD_outBuffer output = { buffOut, buffOutSize, 0 };
			size_t const remainingToFlush = ZSTD_endStream(cstream, &output);   /* close frame */
			if (remainingToFlush) { 
				ret = E_EABORTED;
				break;
			}
			_write(outfile, buffOut, output.pos);
		}
	} while (0);

	if (cstream) ZSTD_freeCStream(cstream);
	if (buffIn) free(buffIn);
	if (buffOut) free(buffOut);

	if (infile != -1)
		_close(infile);
	if (outfile != -1)
		_close(outfile);

	return ret;
}


// ============================================================================
// PackToMem - not supported yet
// ============================================================================
extern "C" HANDLE __stdcall StartMemPack(int options, char* filename) {
	return 0;
}
extern "C" int __stdcall PackToMem(HANDLE hMemPack, char* BufIn, int InLen, int* Taken,
	char* BufOut, int OutLen, int* Written, int* SeekBy) {
	return 0;
}
extern "C" int __stdcall DoneMemPack(HANDLE hMemPack) {
	return 0;
}


// ============================================================================
// GetBackgroundFlags - support some
// ============================================================================
extern "C" int __stdcall GetBackgroundFlags(void) {
	return BACKGROUND_UNPACK | BACKGROUND_PACK;
}

// ============================================================================
// ConfigurePacker + DlgProc
// ============================================================================
extern "C" BOOL __stdcall dlgProc(HWND dlg, UINT  uMsg, WPARAM wParam, LPARAM  lParam) {
	int i;
	wchar_t num[4];
	num[2] = L'\0';
	switch (uMsg) {
	case WM_INITDIALOG:
		for (wchar_t s0 = L' '; s0 <= '1'; s0 += L'1' - L' ') {
			num[0] = s0;
			for (wchar_t ch = L'1'; ch <= L'9'; ch++) {
				num[1] = ch;
				SendDlgItemMessage(dlg, IDC_COMBO, CB_ADDSTRING, 0, (LPARAM)&num);
			}
		}
		i = GetPrivateProfileInt(L"zstdwfx", L"CompressionRate", 3, inifilename);
		SendDlgItemMessage(dlg, IDC_COMBO, CB_SETCURSEL, i - 1, 0);
		return TRUE;
		break;
	case WM_COMMAND:
		switch (wParam) {
		case IDOK:
			wsprintf(num, L"%d", 1 + SendDlgItemMessage(dlg, IDC_COMBO, CB_GETCURSEL, 0, 0));
			WritePrivateProfileString(L"zstdwfx", L"CompressionRate", num, inifilename);
			EndDialog(dlg, 1);
			break;
		case IDCANCEL:
			EndDialog(dlg, 0);
		}
		break;
	}
	return FALSE;
}

extern "C" void __stdcall ConfigurePacker(HWND ParentHandle, HINSTANCE hinstance) {
	DialogBox(hinstance, MAKEINTRESOURCE(IDD_CONFIGDLG), ParentHandle, (DLGPROC)&dlgProc);
}

// ============================================================================
// GetPackerCaps
// ============================================================================
extern "C" int __stdcall GetPackerCaps() {
	return PK_CAPS_NEW | PK_CAPS_OPTIONS
//		| PK_CAPS_MEMPACK 
		| PK_CAPS_BY_CONTENT | PK_CAPS_SEARCHTEXT
		;
}

// ============================================================================
// PackSetDefaultParams
// ============================================================================
extern "C" void __stdcall PackSetDefaultParams(PackDefaultParamStruct* dps) {
	qudConvert(inifilename, dps->DefaultIniName, MAX_PATH - 1);
}

// ============================================================================
// SetProcessDataProc(W)
// ============================================================================
void __stdcall SetProcessDataProcW(int hArcData, tProcessDataProcW pProcessDataProc) {
	struct ZHandle * zh = hArcData == -1 ? &defZH : (struct ZHandle *)hArcData;
	if (!zh) return;
	zh->pprocW = pProcessDataProc;
}
void __stdcall SetProcessDataProc(int hArcData, tProcessDataProc pProcessDataProc) {
	struct ZHandle * zh = hArcData == -1 ? &defZH : (struct ZHandle *)hArcData;
	if (!zh) return;
	zh->pproc = pProcessDataProc;
}

// ============================================================================
// SetChangeVolProc(W)
// ============================================================================
extern "C" void __stdcall SetChangeVolProcW(int hArcData, tChangeVolProcW pChangeVolProcW) {
}
extern "C" void __stdcall SetChangeVolProc(int hArcData, tChangeVolProc pChangeVolProc) {
}

// ============================================================================
// DeleteFiles - not supported - only 1 file
// ============================================================================
extern "C" int __stdcall DeleteFiles(char *PackedFile, char *DeleteList) {
	return E_NOT_SUPPORTED;
}
extern "C" int __stdcall DeleteFilesW(WCHAR *PackedFile, WCHAR *DeleteList) {
	return E_NOT_SUPPORTED;
}

// ============================================================================
// OpenArchiveW - call unicode version
// ============================================================================
extern "C" int __stdcall OpenArchive(tOpenArchiveData* archiveData)
{
	wchar_t arcNameW[MAX_PATH];
	wchar_t cmtBufW[MAX_PATH];

	qudConvert(archiveData->ArcName, arcNameW, MAX_PATH - 1);
	qudConvert(archiveData->CmtBuf, cmtBufW, MAX_PATH - 1);

	tOpenArchiveDataW oad = {
		arcNameW,
		archiveData->OpenMode,
		archiveData->OpenResult,
		cmtBufW,
		archiveData->CmtBufSize,
		archiveData->CmtSize,
		archiveData->CmtState
	};
	return OpenArchiveW(&oad);
}

// ============================================================================
// CanYouHandleThisFile call unicode version
// ============================================================================
extern "C" int __stdcall CanYouHandleThisFile(char* filename) {
	wchar_t wf[MAX_PATH];
	qudConvert(filename, wf, MAX_PATH - 1);
	return CanYouHandleThisFileW(wf);
}

// ============================================================================
// PackFiles
// ============================================================================
extern "C" int __stdcall PackFiles(char *packedFile, char *subPath, char *srcPath,
	char *addList, int flags) {

	char *p = addList;
	// check for one file
	if (!p || !p[0]) return E_NO_FILES;
	while (p[0]) p++;
	p++;
	if (p[0]) return E_TOO_MANY_FILES;

	wchar_t wpackedFile[MAX_PATH];
	wchar_t wsubPath[MAX_PATH];
	wchar_t wsrcPath[MAX_PATH];
	wchar_t waddList[MAX_PATH + 1];

	qudConvert(wpackedFile, packedFile, MAX_PATH);
	qudConvert(wsubPath, subPath, MAX_PATH);
	qudConvert(wsrcPath, srcPath, MAX_PATH);
	qudConvert(waddList, addList, MAX_PATH);
	waddList[wcslen(waddList) + 1] = 0;

	return PackFilesW(wpackedFile, wsubPath, wsrcPath, waddList, flags);
}

// ============================================================================
// ProcessFile
// ============================================================================
extern "C" int __stdcall ProcessFile(int hArcData, int operation, char * destPath, char* destName) {
	wchar_t destPathw[MAX_PATH];
	wchar_t destNamew[MAX_PATH];

	qudConvert(destPathw, destPath, MAX_PATH);
	qudConvert(destNamew, destName, MAX_PATH);

	return ProcessFileW(hArcData, operation, destPathw, destNamew);
}
