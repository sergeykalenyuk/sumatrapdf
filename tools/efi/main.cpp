/* Copyright 2013 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "BaseUtil.h"
#include "BitManip.h"
#include "Dict.h"

#include "Dia2Subset.h"
//#include <Dia2.h>
#include "Util.h"

#include <DbgHelp.h>

// This constant may be missing from DbgHelp.h.  See the documentation for
// IDiaSymbol::get_undecoratedNameEx.
#ifndef UNDNAME_NO_ECSU
#define UNDNAME_NO_ECSU 0x8000  // Suppresses enum/class/struct/union.
#endif  // UNDNAME_NO_ECSU

// Add SymTag to get the DIA name (i.e. Null => SymTagNull). You can then google
// that name to figure out what it means
//
// must match order of enum SymTagEnum in Dia2Subset.h
const char *g_symTypeNames[SymTagMax] = {
    "Null",
    "Exe",
    "Compiland",
    "CompilandDetails",
    "CompilandEnv",
    "Function",
    "Block",
    "Data",
    "Annotation",
    "Label",
    "PublicSymbol",
    "UDT",
    "Enum",
    "FunctionType",
    "PointerType",
    "ArrayType",
    "BaseType",
    "Typedef",
    "BaseClass",
    "Friend",
    "FunctionArgType",
    "FuncDebugStart",
    "FuncDebugEnd",
    "UsingNamespace",
    "VTableShape",
    "VTable",
    "Custom",
    "Thunk",
    "CustomType",
    "ManagedType",
    "Dimension"
};

const char *g_symTypeNamesCompact[SymTagMax] = {
    "N",        // Null
    "Exe",      // Exe
    "C",        // Compiland
    "CD",       // CompilandDetails
    "CE",       // CompilandEnv
    "F",        // Function
    "B",        // Block
    "D",        // Data
    "A",        // Annotation
    "L",        // Label
    "P",        // PublicSymbol
    "U",        // UDT
    "E",        // Enum
    "FT",       // FunctionType
    "PT",       // PointerType
    "AT",       // ArrayType
    "BT",       // BaseType
    "T",        // Typedef
    "BC",       // BaseClass
    "Friend",   // Friend
    "FAT",      // FunctionArgType
    "FDS",      // FuncDebugStart
    "FDE",      // FuncDebugEnd
    "UN",       // UsingNamespace
    "VTS",      // VTableShape
    "VT",       // VTable
    "Custom",   // Custom
    "Thunk",    // Thunk
    "CT",       // CustomType
    "MT",       // ManagedType
    "Dim"       // Dimension
};

static str::Str<char> g_report;
static StringInterner g_strInterner;
static StringInterner g_typesSeen;

static bool           g_dumpSections = false;
static bool           g_dumpSymbols = false;
static bool           g_dumpTypes = false;
// TODO: possibly just remove the flag and always do compact
static bool           g_compact = true;

static void SysFreeStringSafe(BSTR s)
{
    if (s)
        SysFreeString(s);
}

static void UnkReleaseSafe(IUnknown *i)
{
    if (i)
        i->Release();
}

static int InternString(const char *s)
{
    return g_strInterner.Intern(s);
}

static void GetInternedStringsReport(str::Str<char>& resOut)
{
    resOut.Append("Strings:\n");
    int n = g_strInterner.StringsCount();
    for (int i = 0; i < n; i++) {
        resOut.AppendFmt("%d|%s\n", i, g_strInterner.GetByIndex(i));
    }
    resOut.Append("\n");
}

static const char *GetSymTypeName(int i)
{
    if (i >= SymTagMax)
        return "<unknown type>";
    if (g_compact)
        return g_symTypeNamesCompact[i];
    else
        return g_symTypeNames[i];
}

static const char *GetSectionType(IDiaSectionContrib *item)
{
    BOOL code=FALSE,initData=FALSE,uninitData=FALSE;
    item->get_code(&code);
    item->get_initializedData(&initData);
    item->get_uninitializedData(&uninitData);

    if (code && !initData && !uninitData)
        return "C";
    if (!code && initData && !uninitData)
        return "D";
    if (!code && !initData && uninitData)
        return "B";
    return "U";
}

// the result doesn't have to be free()d but is only valid until the next call to this function
static const char *GetObjFileName(IDiaSectionContrib *item)
{
    static str::Str<char> strTmp;

    IDiaSymbol *    compiland = 0;
    BSTR            objFileName = 0;

    item->get_compiland(&compiland);
    if (!compiland)
        return "<noobjfile>";

    compiland->get_name(&objFileName);
    compiland->Release();

    BStrToString(strTmp, objFileName, "<noobjfile>");
    SysFreeStringSafe(objFileName);
    return strTmp.Get();
}

static void DumpSection(IDiaSectionContrib *item)
{
    DWORD           sectionNo;
    DWORD           offset;
    DWORD           length;

    item->get_addressSection(&sectionNo);
    item->get_addressOffset(&offset);
    item->get_length(&length);

    //DWORD compilandId;
    //item->get_compilandId(&compilandId);

    const char *sectionType = GetSectionType(item);
    const char *objFileName = GetObjFileName(item);

    if (g_compact) {
        // type | sectionNo | length | offset | objFileId
        int objFileId = InternString(objFileName);
        g_report.AppendFmt("%s|%d|%d|%d|%d\n", sectionType, sectionNo, length, offset, objFileId);
    } else {
        // type | sectionNo | length | offset | objFile
        g_report.AppendFmt("%s|%d|%d|%d|%s\n", sectionType, sectionNo, length, offset, objFileName);
    }
}

static void AddReportSepLine()
{
    if (g_report.Count() > 0)
        g_report.Append("\n");
}

static char g_spacesBuf[256];
static const char *spaces(int deep)
{
    if (0 == deep)
        return "";
    if (1 == deep)
        return "  ";
    if (2 == deep)
        return "    ";
    if (3 == deep)
        return "      ";
    if (4 == deep)
        return "        ";
    deep = deep * 2;
    for (int i = 0; i < deep; i++) {
        g_spacesBuf[i] = ' ';
    }
    g_spacesBuf[deep] = 0;
    return (const char*)g_spacesBuf;
}

static const char *GetUdtType(IDiaSymbol *symbol)
{
    DWORD kind = (DWORD)-1;
    if (FAILED(symbol->get_udtKind(&kind)))
        return "<unknown udt kind>";
    if (UdtStruct == kind)
        return "struct";
    if (UdtClass == kind)
        return "class";
    if (UdtUnion == kind)
        return "union";
    return "<unknown udt kind>";
}

// the result doesn't have to be free()d but is only valid until the next call to this function
static const char *GetTypeName(IDiaSymbol *symbol)
{
    static str::Str<char> strTmp;
    BSTR name = NULL;
    symbol->get_name(&name);
    BStrToString(strTmp, name, "<noname>", true);
    SysFreeStringSafe(name);
    return strTmp.Get();
}

static void DumpType(IDiaSymbol *symbol, int deep)
{
    IDiaEnumSymbols *   enumChilds = NULL;
    HRESULT             hr;
    const char *        nameStr = NULL;
    const char *        type;
    LONG                offset;
    ULONGLONG           length;
    ULONG               celt = 0;
    ULONG               symtag;
    DWORD               locType;
    bool                typeSeen;

#if 0
    if (deep > 2)
        return;
#endif

    if (symbol->get_symTag(&symtag) != S_OK)
        return;

    nameStr = GetTypeName(symbol);

    g_typesSeen.Intern(nameStr, &typeSeen);
    if (typeSeen)
        return;

    symbol->get_length(&length);
    symbol->get_offset(&offset);

    if (SymTagData == symtag) {
        if (symbol->get_locationType(&locType) != S_OK)
            return; // must be a symbol in optimized code

        // TODO: use get_offsetInUdt (http://msdn.microsoft.com/en-us/library/dd997149.aspx) ?
        // TODO: use get_type (http://msdn.microsoft.com/en-US/library/cwx3656b(v=vs.80).aspx) ?
        // TODO: see what else we can get http://msdn.microsoft.com/en-US/library/w8ae4k32(v=vs.80).aspx
        if (LocIsThisRel == locType) {
            g_report.AppendFmt("%s%s|%d\n", spaces(deep), nameStr, (int)offset);
        }
    } else if (SymTagUDT == symtag) {
        // TODO: why is it always "struct" even for classes?
        type = GetUdtType(symbol);
        g_report.AppendFmt("%s%s|%s|%d\n", spaces(deep), type, nameStr, (int)length);
        hr = symbol->findChildren(SymTagNull, NULL, nsNone, &enumChilds);
        if (!SUCCEEDED(hr))
            return;
        IDiaSymbol* child;
        while (SUCCEEDED(enumChilds->Next(1, &child, &celt)) && (celt == 1))
        {
            DumpType(child, deep+1);
            child->Release();
        }
        enumChilds->Release();
    } else {
        if (symbol->get_locationType(&locType) != S_OK)
            return; // must be a symbol in optimized code
        // TODO: assert?
    }
}

static void DumpTypes(IDiaSession *session)
{
    IDiaSymbol *        globalScope = NULL;
    IDiaEnumSymbols *   enumSymbols = NULL;
    IDiaSymbol *        symbol = NULL;

    HRESULT hr = session->get_globalScope(&globalScope);
    if (FAILED(hr))
        return;

    AddReportSepLine();
    g_report.Append("Types:\n");

    DWORD flags = nsfCaseInsensitive|nsfUndecoratedName; // nsNone ?
    hr = globalScope->findChildren(SymTagUDT, 0, flags, &enumSymbols);
    if (FAILED(hr))
        goto Exit;

    ULONG celt = 0;
    for (;;)
    {
        hr = enumSymbols->Next(1, &symbol, &celt);
        if (FAILED(hr) || (celt != 1))
            break;
        DumpType(symbol, 0);
        symbol->Release();
    }

Exit:
    UnkReleaseSafe(enumSymbols);
    UnkReleaseSafe(globalScope);
}

// the result doesn't have to be free()d but is only valid until the next call to this function
static const char *GetUndecoratedSymbolName(IDiaSymbol *symbol)
{
    static str::Str<char> strTmp;

    BSTR name = NULL;

#if 0
    DWORD undecorateOptions = UNDNAME_COMPLETE;
#else
    DWORD undecorateOptions =  UNDNAME_NO_MS_KEYWORDS |
                                UNDNAME_NO_FUNCTION_RETURNS |
                                UNDNAME_NO_ALLOCATION_MODEL |
                                UNDNAME_NO_ALLOCATION_LANGUAGE |
                                UNDNAME_NO_THISTYPE |
                                UNDNAME_NO_ACCESS_SPECIFIERS |
                                UNDNAME_NO_THROW_SIGNATURES |
                                UNDNAME_NO_MEMBER_TYPE |
                                UNDNAME_NO_RETURN_UDT_MODEL |
                                UNDNAME_NO_ECSU;
#endif

    if (S_OK == symbol->get_undecoratedNameEx(undecorateOptions, &name)) {
        BStrToString(strTmp, name, "", true);
        if (str::Eq(strTmp.Get(), "`string'"))
            return "*str";
        strTmp.Replace("(void)", "()");
        // more ideas for undecoration:
        // http://google-breakpad.googlecode.com/svn/trunk/src/common/windows/pdb_source_line_writer.cc
    } else {
        // Unfortunately it does happen that get_undecoratedNameEx() fails
        // e.g. for RememberDefaultWindowPosition() in Sumatra code
        symbol->get_name(&name);
        BStrToString(strTmp, name, "<noname>", true);
    }
    SysFreeStringSafe(name);

    return strTmp.Get();
}

static void DumpSymbol(IDiaSymbol *symbol)
{
    DWORD               section, offset, rva;
    DWORD               dwTag;
    enum SymTagEnum     tag;
    ULONGLONG           length = 0;
    BSTR                undecoratedName = NULL;
    //BSTR                srcFileName = NULL;
    const char *        typeName = NULL;
    const char *        nameStr = NULL;

    symbol->get_symTag(&dwTag);
    tag = (enum SymTagEnum)dwTag;
    typeName = GetSymTypeName(tag);

    symbol->get_relativeVirtualAddress(&rva);
    symbol->get_length(&length);
    symbol->get_addressSection(&section);
    symbol->get_addressOffset(&offset);

    // get length from type for data
    if (tag == SymTagData)
    {
        IDiaSymbol *type = NULL;
        if (symbol->get_type(&type) == S_OK) // no SUCCEEDED test as may return S_FALSE!
        {
            if (FAILED(type->get_length(&length)))
                length = 0;
            type->Release();
        }
        else
            length = 0;
    }

    nameStr = GetUndecoratedSymbolName(symbol);
    // type | section | length | offset | rva | name
    g_report.AppendFmt("%s|%d|%d|%d|%d|%s\n", typeName, (int)section, (int)length, (int)offset, (int)rva, nameStr);

    SysFreeStringSafe(undecoratedName);
}

static void DumpSymbols(IDiaSession *session)
{
    HRESULT                 hr;
    IDiaEnumSymbolsByAddr * enumByAddr = NULL;
    IDiaSymbol *            symbol = NULL;

    hr = session->getSymbolsByAddr(&enumByAddr);
    if (!SUCCEEDED(hr))
        goto Exit;

    // get first symbol to get first RVA (argh)
    hr = enumByAddr->symbolByAddr(1, 0, &symbol);
    if (!SUCCEEDED(hr))
        goto Exit;

    DWORD rva;
    hr = symbol->get_relativeVirtualAddress(&rva);
    if (S_OK != hr)
        goto Exit;

    symbol->Release();
    symbol = NULL;

    // enumerate by rva
    hr = enumByAddr->symbolByRVA(rva, &symbol);
    if (!SUCCEEDED(hr))
        goto Exit;

    AddReportSepLine();
    g_report.Append("Symbols:\n");

    ULONG numFetched;
    for (;;)
    {
        DumpSymbol(symbol);
        symbol->Release();
        symbol = NULL;

        hr = enumByAddr->Next(1, &symbol, &numFetched);
        if (FAILED(hr) || (numFetched != 1))
            break;
    }

Exit:
    UnkReleaseSafe(symbol);
    UnkReleaseSafe(enumByAddr);
}

static void DumpSections(IDiaSession *session)
{
    HRESULT             hr;
    IDiaEnumTables *    enumTables = NULL;
    IDiaTable *         secTable = NULL;

    hr = session->getEnumTables(&enumTables);
    if (S_OK != hr)
        return;

    AddReportSepLine();
    g_report.Append("Sections:\n");

    VARIANT vIndex;
    vIndex.vt = VT_BSTR;
    vIndex.bstrVal = SysAllocString(L"Sections");

    hr = enumTables->Item(vIndex, &secTable);
    if (S_OK != hr)
        goto Exit;

    LONG count;

    secTable->get_Count(&count);

    IDiaSectionContrib *item;
    ULONG numFetched;
    for (;;)
    {
        hr = secTable->Next(1,(IUnknown **)&item, &numFetched);
        if (FAILED(hr) || (numFetched != 1))
            break;

        DumpSection(item);
        item->Release();
    }

Exit:
    UnkReleaseSafe(secTable);
    SysFreeStringSafe(vIndex.bstrVal);
    UnkReleaseSafe(enumTables);
}

static void ProcessPdbFile(const char *fileNameA)
{
    HRESULT             hr;
    IDiaDataSource *    dia = NULL;
    IDiaSession *       session = NULL;
    str::Str<char>      report;

    dia = LoadDia();
    if (!dia)
        return;

    ScopedMem<WCHAR> fileName(str::conv::FromAnsi(fileNameA));
    hr = dia->loadDataForExe(fileName, 0, 0);
    if (FAILED(hr)) {
        logf("  failed to load %s or its debug symbols from .pdb file\n", fileNameA);
        goto Exit;
    }

    hr = dia->openSession(&session);
    if (FAILED(hr)) {
        log("  failed to open DIA session\n");
        goto Exit;
    }

    if (g_dumpTypes) {
        DumpTypes(session);
    }

    if (g_dumpSections) {
        DumpSections(session);
    }

    if (g_dumpSymbols) {
        DumpSymbols(session);
    }

    fputs("Format: 1\n", stdout);
    if (g_compact) {
        str::Str<char> res;
        GetInternedStringsReport(res);
        fputs(res.Get(), stdout);
    }
    fputs(g_report.Get(), stdout);

Exit:
    UnkReleaseSafe(session);
}

static char *g_fileName = NULL;

static bool ParseCommandLine(int argc, char **argv)
{
    char *s;
    for (int i=0; i<argc; i++) {
        s = argv[i];
        if (str::EqI(s, "-compact"))
            g_compact = true;
        else if (str::EqI(s, "-sections"))
            g_dumpSections = true;
        else if (str::EqI(s, "-symbols"))
            g_dumpSymbols = true;
        else if (str::EqI(s, "-types"))
            g_dumpTypes = true;
        else {
            if (g_fileName != NULL)
                goto InvalidCmdLine;
            g_fileName = s;
        }
    }
    if (!g_fileName)
        goto InvalidCmdLine;

    if (!g_dumpSections && !g_dumpSymbols && !g_dumpTypes) {
        // no options specified so use default settings:
        // dump all information in non-compact way
        g_dumpSections = true;
        g_dumpSymbols = true;
        // TODO: for now don't dump types by default
        //g_dumpTypes = true;
    }

    return true;

InvalidCmdLine:
    printf("%s", "Usage: efi [-sections] [-symbols] [-types] <exefile>\n");
    return false;
}

int main(int argc, char** argv)
{
    ScopedCom comInitializer;
    if (!ParseCommandLine(argc-1, &argv[1]))
        return 1;
    ProcessPdbFile(g_fileName);
    return 0;
}