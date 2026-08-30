#include "BaseException.h"
#include <iomanip>
#include <string>
#include <sstream>
#include <iostream>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <mutex>

#include "libslic3r_version.h"

static std::string g_log_folder;
static std::atomic<int> g_crash_log_count = 0;
static std::mutex g_dump_mutex;

CBaseException::CBaseException(HANDLE hProcess, WORD wPID, LPCTSTR lpSymbolPath, PEXCEPTION_POINTERS pEp):
	CStackWalker(hProcess, wPID, lpSymbolPath)
{
	if (NULL != pEp)
	{
		m_pEp = new EXCEPTION_POINTERS;
		CopyMemory(m_pEp, pEp, sizeof(EXCEPTION_POINTERS));
	}
	output_file = new boost::nowide::ofstream();
	std::time_t t = std::time(0);
	std::tm* now_time = std::localtime(&t);
	std::stringstream buf;

	if (!g_log_folder.empty()) {
		buf << std::put_time(now_time, "crash_%a_%b_%d_%H_%M_%S_") <<g_crash_log_count++ <<".log";
		auto log_folder = (boost::filesystem::path(g_log_folder) / "log").make_preferred();
		if (!boost::filesystem::exists(log_folder)) {
		    boost::filesystem::create_directory(log_folder);
	    }
		auto crash_log_path = boost::filesystem::path(log_folder / buf.str()).make_preferred();
		std::string log_filename = crash_log_path.string();
		output_file->open(log_filename, std::ios::out | std::ios::app);

		// Output app build info in crash log so we could look for the correct PDB files
        OutputString(_T("%s\n\n"), _T(SLIC3R_APP_DISPLAY_NAME " " INLONGSLICER_VERSION " Build " GIT_COMMIT_HASH));
	}
}

CBaseException::~CBaseException(void)
{
	if (output_file) {
		output_file->close();
		delete output_file;
	}
}


//BBS set crash log folder
void CBaseException::set_log_folder(std::string log_folder)
{
	g_log_folder = log_folder;
}

void CBaseException::OutputString(LPCTSTR lpszFormat, ...)
{
	TCHAR szBuf[2048] = _T("");
	va_list args;
	va_start(args, lpszFormat);
	_vsntprintf_s(szBuf, 2048, lpszFormat, args);
	va_end(args);

	//WriteConsole(GetStdHandle(STD_OUTPUT_HANDLE), szBuf, _tcslen(szBuf), NULL, NULL);

	//output it to the current directory of binary
    std::string output_str = static_cast<const char*>(textconv_helper::T2A_(szBuf));
	*output_file << output_str;
	output_file->flush();
}

void CBaseException::ShowLoadModules()
{
	LoadSymbol();
	LPMODULE_INFO pHead = GetLoadModules();
	LPMODULE_INFO pmi = pHead;

	TCHAR szBuf[MAX_COMPUTERNAME_LENGTH] = _T("");
	DWORD dwSize = MAX_COMPUTERNAME_LENGTH;
	GetUserName(szBuf, &dwSize);
	OutputString(_T("Current User:%s\r\n"), szBuf);
	OutputString(_T("BaseAddress:\tSize:\tName\tPath\tSymbolPath\tVersion\r\n"));
	while (NULL != pmi)
	{
		OutputString(_T("%08x\t%d\t%s\t%s\t%s\t%s\r\n"), (unsigned long)(pmi->ModuleAddress), pmi->dwModSize, pmi->szModuleName, pmi->szModulePath, pmi->szSymbolPath, pmi->szVersion);
		pmi = pmi->pNext;
	}

	FreeModuleInformations(pHead);
}

void CBaseException::ShowCallstack(HANDLE hThread, const CONTEXT* context)
{
	OutputString(_T("Show CallStack:\n"));
    LPSTACKINFO phead = StackWalker(hThread, context);

	// Show RVA of each call stack, so we can locate the symbol using pdb file
	// To show the symbol, load the <szFaultingModule> in WinDBG with pdb file, then type the following commands:
	// > lm                                                                   which gives you the start address of each module, as well as module names
	// > !dh <module name>                                                    list all module headers. Find the <virtual address> of the section given by
	//                                                                        the <section> output in the crash log
	// > ln <module start address> + <section virtual address> + <offset>     reveals the debug symbol
    OutputString(_T("\nLogical Address:\n"));
    TCHAR szFaultingModule[MAX_PATH];
    DWORD section, offset;
    for (LPSTACKINFO ps = phead; ps != nullptr; ps = ps->pNext) {
        if (GetLogicalAddress((PVOID) ps->szFncAddr, szFaultingModule, sizeof(szFaultingModule), section, offset)) {
            OutputString(_T("0x%X 0x%X:0x%X %s\n"), ps->szFncAddr, section, offset, szFaultingModule);
        } else {
            OutputString(_T("0x%X Unknown\n"), ps->szFncAddr);
        }
    }

	FreeStackInformations(phead);
}

void CBaseException::ShowExceptionResoult(DWORD dwExceptionCode)
{
	OutputString(_T("Exception Code :%08x "), dwExceptionCode);
// BBS: to be checked
#if 1
	switch (dwExceptionCode)
	{
	case EXCEPTION_ACCESS_VIOLATION:
		{
			//OutputString(_T("ACCESS_VIOLATION(%s)\r\n"), _T("嚙踝蕭迡嚙褒瘀蕭嚙誹湛蕭"));
			OutputString(_T("ACCESS_VIOLATION\r\n"));
		}
		return ;
	case EXCEPTION_DATATYPE_MISALIGNMENT:
		{
			//OutputString(_T("DATATYPE_MISALIGNMENT(%s)\r\n"), _T("嚙賠喉蕭嚙踝蕭芞嚙誹莎蕭盓嚙誰塚蕭嚙踝蕭嚙諉莎蕭嚙踝蕭炩嚙請棒湛蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭"));
			OutputString(_T("DATATYPE_MISALIGNMENT\r\n"));
		}
		return ;
	case EXCEPTION_BREAKPOINT:
		{
			//OutputString(_T("BREAKPOINT(%s)\r\n"), _T("嚙踝蕭嚙踝蕭珨嚙踝蕭嚙誕蛛蕭"));
			OutputString(_T("BREAKPOINT\r\n"));
		}
		return ;
	case EXCEPTION_SINGLE_STEP:
		{
			//OutputString(_T("SINGLE_STEP(%s)\r\n"), _T("嚙踝蕭嚙踝蕭")); //珨嚙踝蕭嚙褒瘀蕭嚙踝蕭嚙誹蛛蕭嚙踝蕭嚙蝓潘蕭嚙踝蕭
			OutputString(_T("SINGLE_STEP\r\n"));
		}
		return ;
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		{
			//OutputString(_T("ARRAY_BOUNDS_EXCEEDED(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭嚙諂踝蕭嚙?));
			OutputString(_T("ARRAY_BOUNDS_EXCEEDED\r\n"));
		}
		return ;
	case EXCEPTION_FLT_DENORMAL_OPERAND:
		{
			//OutputString(_T("FLT_DENORMAL_OPERAND(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙課鳴蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭璉穿蕭嚙踝蕭嚙踝蕭譫嚙踝蕭嚙踝蕭嚙踝蕭瑀嚙踝蕭嚙褊?)); //嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭
			OutputString(_T("FLT_DENORMAL_OPERAND\r\n"));
		}
		return ;
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
		{
			//OutputString(_T("FLT_DIVIDE_BY_ZERO(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭0嚙踝蕭嚙踝蕭"));
			OutputString(_T("FLT_DIVIDE_BY_ZERO\r\n"));
		}
		return ;
	case EXCEPTION_FLT_INEXACT_RESULT:
		{
			//OutputString(_T("FLT_INEXACT_RESULT(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙衝踝蕭嚙踝蕭瑀嚙踝蕭嚙褊?)); //嚙豬瘀蕭嚙踝蕭尨珨嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭怮苤嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭尨嚙衝瘀蕭峓, 嚙踝蕭嚙踝蕭眳嚙踝蕭嚙踝蕭嚙踝蕭躅嚙踝蕭嚙趣都
			OutputString(_T("FLT_INEXACT_RESULT\r\n"));
		}
		return ;
	case EXCEPTION_FLT_INVALID_OPERATION:
		{
			//OutputString(_T("FLT_INVALID_OPERATION(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙趣都"));
			OutputString(_T("FLT_INVALID_OPERATION\r\n"));
		}
		return ;
	case EXCEPTION_FLT_OVERFLOW:
		{
			//OutputString(_T("FLT_OVERFLOW(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙誰賂蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙諉佗蕭嚙踝蕭芚嚙踝蕭嚙踝蕭硉"));
			OutputString(_T("FLT_OVERFLOW\r\n"));
		}
		return ;
	case EXCEPTION_FLT_STACK_CHECK:
		{
			//OutputString(_T("STACK_CHECK(%s)\r\n"), _T("梬埣嚙踝蕭嚙踝蕭嚙調鳴蕭嚙踝蕭嚙踝蕭嚙踝蕭"));
			OutputString(_T("STACK_CHECK\r\n"));
		}
		return ;
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
		{
			//OutputString(_T("INT_DIVIDE_BY_ZERO(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭0嚙趣都"));
			OutputString(_T("INT_DIVIDE_BY_ZERO\r\n"));
		}
		return ;
	case EXCEPTION_INVALID_HANDLE:
		{
			//OutputString(_T("INVALID_HANDLE(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙請?));
			OutputString(_T("INVALID_HANDLE\r\n"));
		}
		return ;
	case EXCEPTION_PRIV_INSTRUCTION:
		{
			//OutputString(_T("PRIV_INSTRUCTION(%s)\r\n"), _T("嚙賠喉蕭嚙踝蕭芞硒嚙請蛛蕭嚙踝蕭嚙踝蕭耀宒嚙踝蕭盓嚙誰蛛蕭硌嚙踝蕭"));
			OutputString(_T("PRIV_INSTRUCTION\r\n"));
		}
		return ;
	case EXCEPTION_IN_PAGE_ERROR:
		{
			//OutputString(_T("IN_PAGE_ERROR(%s)\r\n"), _T("嚙賠喉蕭嚙踝蕭芞嚙踝蕭嚙踝蕭帤嚙踝蕭嚙諍蛛蕭嚙踝蕭嚙踝蕭嚙誹湛蕭珜嚙踝蕭嚙賠莎蕭嚙豌潘蕭嚙諍蛛蕭嚙踝蕭嚙踝蕭嚙誹湛蕭珜"));
			OutputString(_T("IN_PAGE_ERROR\r\n"));
		}
		return ;
	case EXCEPTION_ILLEGAL_INSTRUCTION:
		{
			//OutputString(_T("ILLEGAL_INSTRUCTION(%s)\r\n"), _T("嚙賠喉蕭嚙踝蕭芞硒嚙踝蕭嚙踝蕭虴硌嚙踝蕭"));
			OutputString(_T("ILLEGAL_INSTRUCTION\r\n"));
		}
		return ;
	case EXCEPTION_NONCONTINUABLE_EXCEPTION:
		{
			//OutputString(_T("NONCONTINUABLE_EXCEPTION(%s)\r\n"), _T("嚙賠喉蕭嚙踝蕭芞嚙踝蕭珨嚙踝蕭嚙踝蕭嚙褕潘蕭嚙踝蕭硒嚙請蛛蕭嚙趣都嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙誰湛蕭嚙?));
			OutputString(_T("NONCONTINUABLE_EXCEPTION\r\n"));
		}
		return ;
	case EXCEPTION_STACK_OVERFLOW:
		{
			//OutputString(_T("STACK_OVERFLOW(%s)\r\n"), _T("梬嚙踝蕭嚙?));
			OutputString(_T("STACK_OVERFLOW\r\n"));
		}
		return ;
	case EXCEPTION_INVALID_DISPOSITION:
		{
			//OutputString(_T("INVALID_DISPOSITION(%s)\r\n"), _T("嚙趣都嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙趣都嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭珨嚙踝蕭嚙踝蕭虴嚙踝蕭嚙踝蕭")); //妏嚙衛詢潘蕭嚙踝蕭嚙諂梧蕭迡嚙衝喉蕭嚙踝蕭嚙踝蕭堈嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭麭?
			OutputString(_T("INVALID_DISPOSITION\r\n"));
		}
		return ;
	case EXCEPTION_FLT_UNDERFLOW:
		{
			//OutputString(_T("FLT_UNDERFLOW(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭硌嚙踝蕭苤嚙踝蕭嚙踝蕭茼嚙踝蕭嚙談蛛蕭嚙踝蕭苤硉"));
			OutputString(_T("FLT_UNDERFLOW\r\n"));
		}
		return ;
	case EXCEPTION_INT_OVERFLOW:
		{
			//OutputString(_T("INT_OVERFLOW(%s)\r\n"), _T("嚙踝蕭嚙踝蕭嚙踝蕭嚙踝蕭埣嚙踝蕭"));
			OutputString(_T("INT_OVERFLOW\r\n"));
		}
		return ;
	}

	TCHAR szBuffer[512] = { 0 };

	FormatMessage(  FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE,
		GetModuleHandle( _T("NTDLL.DLL") ),
		dwExceptionCode, 0, szBuffer, sizeof( szBuffer ), 0 );

	OutputString(_T("%s"), szBuffer);
	OutputString(_T("\r\n"));
#endif
}

LONG WINAPI CBaseException::UnhandledExceptionFilter(PEXCEPTION_POINTERS pExceptionInfo )
{
	if (pExceptionInfo->ExceptionRecord->ExceptionCode < 0x80000000
		//BBS: Load project on computers with SDC may trigger this exception (in ShowModal()),
		//     It's not fatal and should be ignored, or there will be lots of meaningless crash logs
		|| pExceptionInfo->ExceptionRecord->ExceptionCode==0xe0434352)
		//BBS: ignore the exception when copy preset
		//|| pExceptionInfo->ExceptionRecord->ExceptionCode==0xe06d7363)
	{
		//BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": got an ExceptionCode %1%, skip it!") % pExceptionInfo->ExceptionRecord->ExceptionCode;
		return EXCEPTION_CONTINUE_SEARCH;
	}
    g_dump_mutex.lock();
	CBaseException base(GetCurrentProcess(), GetCurrentProcessId(), NULL, pExceptionInfo);
	base.ShowExceptionInformation();
    g_dump_mutex.unlock();

	return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI CBaseException::UnhandledExceptionFilter2(PEXCEPTION_POINTERS pExceptionInfo )
{
	CBaseException base(GetCurrentProcess(), GetCurrentProcessId(), NULL, pExceptionInfo);
	base.ShowExceptionInformation();

	return EXCEPTION_CONTINUE_SEARCH;
}

BOOL CBaseException::GetLogicalAddress(
	PVOID addr, PTSTR szModule, DWORD len, DWORD& section, DWORD& offset )
{
	MEMORY_BASIC_INFORMATION mbi;

	if ( !VirtualQuery( addr, &mbi, sizeof(mbi) ) )
		return FALSE;

	DWORD_PTR hMod = (DWORD_PTR)mbi.AllocationBase;

	if ( !GetModuleFileName( (HMODULE)hMod, szModule, len ) )
		return FALSE;

	if (!hMod)
		return FALSE;

	PIMAGE_DOS_HEADER pDosHdr = (PIMAGE_DOS_HEADER)hMod;
	PIMAGE_NT_HEADERS pNtHdr = (PIMAGE_NT_HEADERS)(hMod + pDosHdr->e_lfanew);
	PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION( pNtHdr );

	DWORD_PTR rva = (DWORD_PTR)addr - hMod;

	//嚙踝蕭嚙賬絞嚙踝蕭硊嚙誹菴潘蕭嚙踝蕭嚙踝蕭
	for (unsigned i = 0; i < pNtHdr->FileHeader.NumberOfSections; i++, pSection++ )
	{
		DWORD sectionStart = pSection->VirtualAddress;
		DWORD sectionEnd = sectionStart + std::max(pSection->SizeOfRawData, pSection->Misc.VirtualSize);

		if ( (rva >= sectionStart) && (rva <= sectionEnd) )
		{
			section = i+1;
			offset = rva - sectionStart;
			return TRUE;
		}
	}

	return FALSE;   // Should never get here!
}

void CBaseException::ShowRegistorInformation(PCONTEXT pCtx)
{
#if defined(_M_IX86) // Intel Only!
	OutputString( _T("\nRegisters:\r\n") );

	OutputString(_T("EAX:%08X\r\nEBX:%08X\r\nECX:%08X\r\nEDX:%08X\r\nESI:%08X\r\nEDI:%08X\r\n"),
		pCtx->Eax, pCtx->Ebx, pCtx->Ecx, pCtx->Edx, pCtx->Esi, pCtx->Edi );

	OutputString( _T("CS:EIP:%04X:%08X\r\n"), pCtx->SegCs, pCtx->Eip );
	OutputString( _T("SS:ESP:%04X:%08X  EBP:%08X\r\n"),pCtx->SegSs, pCtx->Esp, pCtx->Ebp );
	OutputString( _T("DS:%04X  ES:%04X  FS:%04X  GS:%04X\r\n"), pCtx->SegDs, pCtx->SegEs, pCtx->SegFs, pCtx->SegGs );
	OutputString( _T("Flags:%08X\r\n"), pCtx->EFlags );
#elif defined(_M_X64)
    OutputString(_T("\nRegisters:\r\n"));

	OutputString(_T("RAX:%016llX\r\nRBX:%016llX\r\nRCX:%016llX\r\nRDX:%016llX\r\nRSI:%016llX\r\nRDI:%016llX\r\n"),
		pCtx->Rax, pCtx->Rbx, pCtx->Rcx, pCtx->Rdx, pCtx->Rsi, pCtx->Rdi );

	OutputString(_T("R8:%016llX\r\nR9:%016llX\r\nR10:%016llX\r\nR11:%016llX\r\nR12:%016llX\r\nR13:%016llX\r\nR14:%016llX\r\nR15:%016llX\r\n"),
		pCtx->R8, pCtx->R9, pCtx->R10, pCtx->R11, pCtx->R12, pCtx->R13, pCtx->R14, pCtx->R15 );

    OutputString(_T("CS:RIP:%04X:%016llX\r\n"), pCtx->SegCs, pCtx->Rip);
    OutputString(_T("SS:RSP:%04X:%016llX  RBP:%016llX\r\n"), pCtx->SegSs, pCtx->Rsp, pCtx->Rbp);
    OutputString(_T("DS:%04X  ES:%04X  FS:%04X  GS:%04X\r\n"), pCtx->SegDs, pCtx->SegEs, pCtx->SegFs, pCtx->SegGs);
    OutputString(_T("Flags:%08X\r\n"), pCtx->EFlags);
#elif defined(_M_ARM64)
    OutputString(_T("\nRegisters:\r\n"));
    OutputString(_T("PC:%016llX  SP:%016llX  FP:%016llX  LR:%016llX\r\n"),
        (unsigned long long)pCtx->Pc, (unsigned long long)pCtx->Sp,
        (unsigned long long)pCtx->Fp, (unsigned long long)pCtx->Lr);
    OutputString(_T("Cpsr:%08X\r\n"), pCtx->Cpsr);
#endif

	OutputString( _T("\r\n") );
}

void CBaseException::STF(unsigned int ui,  PEXCEPTION_POINTERS pEp)
{
	CBaseException base(GetCurrentProcess(), GetCurrentProcessId(), NULL, pEp);
	throw base;
}

void CBaseException::ShowExceptionInformation()
{
	OutputString(_T("Exceptions:\r\n"));
	ShowExceptionResoult(m_pEp->ExceptionRecord->ExceptionCode);

	OutputString(_T("Exception Flag :0x%x "), m_pEp->ExceptionRecord->ExceptionFlags);
	OutputString(_T("NumberParameters :%ld \n"), m_pEp->ExceptionRecord->NumberParameters);
	for (int i = 0; i < m_pEp->ExceptionRecord->NumberParameters; i++)
	{
		OutputString(_T("Param %d :0x%x \n"), i, m_pEp->ExceptionRecord->ExceptionInformation[i]);
	}
	OutputString(_T("Context :%p \n"), m_pEp->ContextRecord);
#if defined(_M_ARM64)
    OutputString(_T("ContextFlag : 0x%x, Cpsr: 0x%x \n"), m_pEp->ContextRecord->ContextFlags, m_pEp->ContextRecord->Cpsr);
#else
    OutputString(_T("ContextFlag : 0x%x, EFlags: 0x%x \n"), m_pEp->ContextRecord->ContextFlags, m_pEp->ContextRecord->EFlags);
#endif

	TCHAR szFaultingModule[MAX_PATH];
	DWORD section, offset;
	GetLogicalAddress(m_pEp->ExceptionRecord->ExceptionAddress, szFaultingModule, sizeof(szFaultingModule), section, offset );
	OutputString( _T("Fault address:  0x%X 0x%X:0x%X %s\r\n"), m_pEp->ExceptionRecord->ExceptionAddress, section, offset, szFaultingModule );

	ShowRegistorInformation(m_pEp->ContextRecord);

	ShowCallstack(GetCurrentThread(), m_pEp->ContextRecord);
}
