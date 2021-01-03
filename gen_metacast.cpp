//
// WINAMP METACAST
// Harry Denholm, ishani.org 2021
//
// a Winamp plugin for quickly exfiltrating "Currently Playing" track information via global shared memory so that data
// can be acted upon by 3rd party apps - such as producing overlays for streams
//
// NOTE this was written in an afternoon and tested on all of 2 computers, YMMV
//
// shoutout to the Endlesss track club <3
//


// Winamp general-plugin SDK and IPC defs
#include "Winamp/GEN.H"
#include "Winamp/wa_ipc.h"

// usual suspects
#include <cstdlib>
#include <string>
#include <locale>
#include <codecvt>
#include <array>
#include <cinttypes>

// https://github.com/nlohmann/json
#include "json.hpp"
using json = nlohmann::json;

// the kitchen sink
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <VersionHelpers.h>


// ---------------------------------------------------------------------------------------------------------------------
BOOL WINAPI _DllMainCRTStartup( HINSTANCE hInst, ULONG ul_reason_for_call, LPVOID lpReserved )
{
    DisableThreadLibraryCalls( hInst );
    return TRUE;
}

void config();
void quit();
int init();

winampGeneralPurposePlugin plugin =
{
    GPPHDR_VER,
    "Ishani Metadata Caster",
    init,
    config,
    quit,
};

extern "C" __declspec(dllexport) winampGeneralPurposePlugin* winampGetGeneralPurposePlugin()
{
    return &plugin;
}


// ---------------------------------------------------------------------------------------------------------------------
void config()
{

}

// ---------------------------------------------------------------------------------------------------------------------
struct GlobalTransmission
{
    // these 3 constants must be identical across the plugin and all receivers
    static constexpr wchar_t* cGlobalMapppingName   = L"WAIshani_Metacast";
    static constexpr wchar_t* cGlobalMutexName      = L"Global\\WAIshani_MetacastMutex";
    static constexpr uint32_t cSharedBufferSize     = 1024 * 4;

    GlobalTransmission()
        : m_transmissionIndex( 0x6A000000 )         // we use a signal value as magic bytes to check received data
        , m_hFileMapping( INVALID_HANDLE_VALUE )
        , m_hMutex( INVALID_HANDLE_VALUE )
        , m_txBuffer( nullptr )
    {}

    ~GlobalTransmission()
    {
        if ( m_hMutex != INVALID_HANDLE_VALUE )
            CloseHandle( m_hMutex );

        if ( m_txBuffer != nullptr )
            UnmapViewOfFile( m_txBuffer );

        if ( m_hFileMapping != INVALID_HANDLE_VALUE )
            CloseHandle( m_hFileMapping );
    }

    inline bool Init()
    {
        m_hFileMapping = CreateFileMapping( INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, cSharedBufferSize, cGlobalMapppingName );
        m_txBuffer = (uint8_t*)MapViewOfFile( m_hFileMapping, FILE_MAP_WRITE, 0, 0, cSharedBufferSize );
        memset( m_txBuffer, 0, cSharedBufferSize );

        auto pSecDesc = MakeAllowAllSecurityDescriptor();
        if ( pSecDesc )
        {
            SECURITY_ATTRIBUTES SecAttr;
            SecAttr.nLength = sizeof( SECURITY_ATTRIBUTES );
            SecAttr.lpSecurityDescriptor = pSecDesc;
            SecAttr.bInheritHandle = FALSE;

            m_hMutex = CreateMutex( &SecAttr, FALSE, cGlobalMutexName );
            auto dwError = GetLastError();
            LocalFree( pSecDesc );
        }

        return true;
    }

    uint32_t    m_transmissionIndex;    // incremented each time we update the buffer and encoded inside it, 
                                        // so that receiver apps can disregard unchanging contents
    HANDLE      m_hFileMapping;
    HANDLE      m_hMutex;
    uint8_t*    m_txBuffer;

private:

    //
    // http://web.archive.org/web/20151215210112/http://blogs.msdn.com/b/winsdk/archive/2009/11/10/access-denied-on-a-mutex.aspx
    //
    PSECURITY_DESCRIPTOR MakeAllowAllSecurityDescriptor(void)
    {
        WCHAR *pszStringSecurityDescriptor;
        if ( ::IsWindowsVistaOrGreater() )
            pszStringSecurityDescriptor = L"D:(A;;GA;;;WD)(A;;GA;;;AN)S:(ML;;NW;;;ME)";
        else
            pszStringSecurityDescriptor = L"D:(A;;GA;;;WD)(A;;GA;;;AN)";
    
        PSECURITY_DESCRIPTOR pSecDesc;
        if ( !::ConvertStringSecurityDescriptorToSecurityDescriptor(pszStringSecurityDescriptor, SDDL_REVISION_1, &pSecDesc, nullptr) )
            return nullptr;
    
        return pSecDesc;
    }
};

// single instance of our global objects, brought up/down in init() and quit()
GlobalTransmission g_GlobalTransmission;


// ---------------------------------------------------------------------------------------------------------------------


// the data we will extract from WinAmp and serialize out to JSON for consumption elsewhere
struct SongInfo
{
    std::string   trackNum;
    std::string   discNum;
    std::string   BPM;
    std::string   artist;
    std::string   title;
    std::string   album;
    std::string   year;
    std::string   album_artist;
    std::string   length;
    std::string   path;
};

// interrogate Winamp via IPC_GET_EXTENDED_FILE_INFO_HOOKABLE for data about particular metadata
std::string GetWinampMetaInfoByType( HWND ampWnd, const char* infoName, const char* filePath )
{
    static constexpr auto fmtBufSize = 512;
    char fmtBuf[fmtBufSize]{ 0 };

    // need to retrieve bitrate differently
    if ( strcmp( infoName, "Bitrate" ) == 0 )
    {
        const int bitrate = (int)SendMessage( ampWnd, WM_WA_IPC, 1, IPC_GETINFO );

        sprintf_s( fmtBuf, "%d", bitrate );

        return std::string{ fmtBuf };
    }

    extendedFileInfoStruct exInfo;

    char*  which_info_buf         = _strdup( infoName );
    char*  playlist_file_path_buf = _strdup( filePath );

    exInfo.filename       = playlist_file_path_buf;
    exInfo.metadata       = which_info_buf;
    exInfo.ret            = fmtBuf;
    exInfo.retlen         = fmtBufSize;

    if ( !SendMessage( ampWnd, WM_WA_IPC, (WPARAM)&exInfo, IPC_GET_EXTENDED_FILE_INFO_HOOKABLE ) )
    {
        sprintf_s( fmtBuf, "[unknown]" );
    }

    free( which_info_buf );
    free( playlist_file_path_buf );

    return std::string{ fmtBuf };
}

// given a song file, pull out chosen metadata tags into SongInfo fields
void ExtractSongInfo( HWND ampWnd, struct SongInfo* si, const char* path )
{
    const std::array< const char*, 8 > infoString {
        "track",
        "disc",
        "bpm",
        "artist",
        "title",
        "album",
        "year",
        "albumartist",
    };
    const std::array< std::string*, 8 > infoOutputs{
        &si->trackNum,
        &si->discNum,
        &si->BPM,
        &si->artist,
        &si->title,
        &si->album,
        &si->year,
        &si->album_artist,
    };

    for ( auto iter = 0U; iter < infoString.size(); iter++ )
    {
        *infoOutputs[iter] = GetWinampMetaInfoByType( ampWnd, infoString[iter], path );
    }

    // length gets translated from milliseconds
    {
        const auto lengthStr = GetWinampMetaInfoByType( ampWnd, "Length", path );
        const auto iter = atoi( lengthStr.c_str() );

        char lengthFmt[256];
        sprintf_s( lengthFmt, "%d:%02d", iter / 1000 / 60, (iter / 1000) % 60 );

        si->length = std::string{ lengthFmt };
    }

    si->path = path;
}


// ---------------------------------------------------------------------------------------------------------------------

// to be aware of what Winamp is doing, the method is to replace the WndProc on the main window with your own shim that
// then calls the previous one
WNDPROC g_PreviousWndProc   = nullptr;
BOOL g_UnicodeWnd           = FALSE;


// ---------------------------------------------------------------------------------------------------------------------
// called from our wndproc injection when Winamp says the song has changed - eg. start playing/stop/pause/ffwd/rwd
//
void Callback_SongHasChanged( const bool isPlaying )
{
    const int ampTrackListPos = (int)SendMessage( plugin.hwndParent, WM_WA_IPC, 0, IPC_GETLISTPOS );
    const char* ampTrackPath = (const char*)SendMessage( plugin.hwndParent, WM_WA_IPC, (WPARAM)ampTrackListPos, IPC_GETPLAYLISTFILE );
    const char* ampTrackTitle = (const char*)SendMessage( plugin.hwndParent, WM_WA_IPC, (WPARAM)ampTrackListPos, IPC_GETPLAYLISTTITLE );

    // NOTE in my limited testing, I had to use the Unicode version of this call to get an actual path to use, IPC_GETPLAYLISTFILE did not work
    // .. probably I should be switching on g_UnicodeWnd but I haven't spent any time checking
    std::wstring filePathWs{ (const wchar_t*)SendMessage( plugin.hwndParent, WM_WA_IPC, (WPARAM)ampTrackListPos, IPC_GETPLAYLISTFILEW ) };
    
    using convert_type = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_type, wchar_t> converter;
    std::string filePathString = converter.to_bytes( filePathWs );

    // get all the deets we can
    SongInfo si;
    ExtractSongInfo( plugin.hwndParent, &si, filePathString.c_str() );

    try
    {
        // lock access to our memory block; TBD: make this retryable by returning some indication we got blocked, get the windows loop to 
        // re-poke after a delay
        //
        auto waitResult = ::WaitForSingleObject( g_GlobalTransmission.m_hMutex, 250 );
        if ( waitResult == WAIT_OBJECT_0 )
        {
            auto exfilTxID =  (uint32_t*)( g_GlobalTransmission.m_txBuffer );
            auto exfilSize =   (int32_t*)( g_GlobalTransmission.m_txBuffer + sizeof( uint32_t ) );
            auto exfilBuff =      (char*)( g_GlobalTransmission.m_txBuffer + sizeof( uint32_t ) * 2 );

            (*exfilTxID) = g_GlobalTransmission.m_transmissionIndex++;

            // turn out a simple k:v block of json
            json jsonOut = {
                {"playlistIndex",   ampTrackListPos},
                {"trackNum",        si.trackNum},
                {"discNum",         si.discNum},
                {"artist",          si.artist},
                {"title",           si.title},
                {"album",           si.album},
                {"year",            si.year},
                {"BPM",             si.BPM},
                {"length",          si.length},
                {"album_artist",    si.album_artist},
                {"path",            si.path},
                {"isPlaying",       isPlaying},
            };
            std::string jsonString = jsonOut.dump();

            // copy it in and vanish
            strcpy_s(
                exfilBuff, 
                GlobalTransmission::cSharedBufferSize - sizeof( uint32_t ) * 2, 
                jsonString.c_str() );

            (*exfilSize) = jsonString.size();

            ::ReleaseMutex( g_GlobalTransmission.m_hMutex );
        }
    }
    catch ( ... )
    {
        OutputDebugString( L"Winamp Metacast had a panic while trying to update global memory" );
    }
}

// ---------------------------------------------------------------------------------------------------------------------
static LRESULT WINAPI InjectedSubclassProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    if ( lParam == IPC_CB_MISC )
    {
        if ( wParam == IPC_CB_MISC_STATUS )
        {
            const bool isPlaying = SendMessage( plugin.hwndParent, WM_WA_IPC, 0, IPC_ISPLAYING ) == 1;
            Callback_SongHasChanged( isPlaying );
        }
    }

    return (g_UnicodeWnd) ? CallWindowProcW( g_PreviousWndProc, hwnd, msg, wParam, lParam ) : 
                            CallWindowProcA( g_PreviousWndProc, hwnd, msg, wParam, lParam );
}

// ---------------------------------------------------------------------------------------------------------------------
// install our message loop
int init()
{
    if ( g_GlobalTransmission.Init() )
    {
        g_UnicodeWnd        = IsWindowUnicode( plugin.hwndParent );
        g_PreviousWndProc   = (WNDPROC)((g_UnicodeWnd) ? SetWindowLongPtrW( plugin.hwndParent, GWLP_WNDPROC, (LONG_PTR)InjectedSubclassProc ) :
                                                         SetWindowLongPtrA( plugin.hwndParent, GWLP_WNDPROC, (LONG_PTR)InjectedSubclassProc ));
    }
    else
    {
        g_PreviousWndProc = nullptr;
    }

    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// unhook our message loop
void quit()
{
    if ( g_PreviousWndProc != nullptr )
    {
        if ( g_UnicodeWnd )
            SetWindowLongPtrW( plugin.hwndParent, GWLP_WNDPROC, (LONG_PTR)g_PreviousWndProc );
        else
            SetWindowLongPtrA( plugin.hwndParent, GWLP_WNDPROC, (LONG_PTR)g_PreviousWndProc );
    }
}
