// SPDX-License-Identifier: LGPL-2.1-or-later
// A one-use authenticated loopback connection carries all game text. Never argv/files/logs.
struct TextEntrySocket
{
    SOCKET value = INVALID_SOCKET;
    ~TextEntrySocket()
    {
        if ( value != INVALID_SOCKET )
            closesocket( value );
    }
};
struct TextEntryWinsock
{
    bool initialized = false;
    ~TextEntryWinsock()
    {
        if ( initialized )
            WSACleanup();
    }
};
static bool text_entry_transfer( SOCKET socket, void *buffer, DWORD size, bool write )
{
    auto *bytes = static_cast<char *>( buffer );
    DWORD done = 0;
    while ( done < size )
    {
        int count = write ? send( socket, bytes + done, size - done, 0 )
                          : recv( socket, bytes + done, size - done, 0 );
        if ( count <= 0 )
            return false;
        done += count;
    }
    return true;
}
static std::wstring text_entry_quote( const std::wstring &value )
{
    std::wstring out = L"\"";
    size_t slashes = 0;
    for ( WCHAR c : value )
    {
        if ( c == L'\\' )
        {
            ++slashes;
            continue;
        }
        out.append( slashes * ( c == L'\"' ? 2 : 1 ), L'\\' );
        slashes = 0;
        if ( c == L'\"' )
            out += L'\\';
        out += c;
    }
    out.append( slashes * 2, L'\\' );
    out += L'\"';
    return out;
}
static HRESULT text_entry_process( TextEntryContext *ctx )
{
    const std::wstring *fields[] = { &ctx->title, &ctx->description, &ctx->initial };
    for ( const auto *field : fields )
        if ( field->size() > 2097152 )
            return E_INVALIDARG;
    TextEntryWinsock network;
    WSADATA data{};
    if ( WSAStartup( MAKEWORD( 2, 2 ), &data ) )
        return E_FAIL;
    network.initialized = true;
    TextEntrySocket listener, connection;
    listener.value = WSASocketW( AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT );
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    if ( listener.value == INVALID_SOCKET ||
         bind( listener.value, reinterpret_cast<sockaddr *>( &address ), sizeof( address ) ) ||
         listen( listener.value, 1 ) )
        return E_FAIL;
    int addressSize = sizeof( address );
    if ( getsockname( listener.value, reinterpret_cast<sockaddr *>( &address ), &addressSize ) )
        return E_FAIL;
    unsigned char token[32];
    if ( BCryptGenRandom( nullptr, token, sizeof( token ), BCRYPT_USE_SYSTEM_PREFERRED_RNG ) )
        return E_FAIL;
    std::wstring encoded;
    const WCHAR *hex = L"0123456789abcdef";
    for ( unsigned char value : token )
    {
        encoded += hex[value >> 4];
        encoded += hex[value & 15];
    }
    WCHAR directory[32768];
    UINT length = GetSystemDirectoryW( directory, 32768 );
    if ( !length || length >= 32768 )
        return E_FAIL;
    // The runtime stages the helper beside the prefix's system DLLs. Resolve its
    // real host path through Wine rather than depending on a username or install root.
    std::wstring helperDos = std::wstring( directory ) + L"\\nexus-text-entry.py";
    using UnixPathFunction = char *(CDECL *)( LPCWSTR );
    auto unixPath = reinterpret_cast<UnixPathFunction>(
        GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "wine_get_unix_file_name" ) );
    if ( !unixPath )
        return E_NOTIMPL;
    char *host = unixPath( helperDos.c_str() );
    if ( !host )
        return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    std::wstring helper;
    bool valid = text_entry_wide( host, helper );
    HeapFree( GetProcessHeap(), 0, host );
    if ( !valid )
        return E_INVALIDARG;
    std::wstring application = std::wstring( directory ) + L"\\start.exe";
    std::wstring command = text_entry_quote( application ) + L" /wait /unix /usr/bin/python3 " +
                           text_entry_quote( helper ) + L" --port " +
                           std::to_wstring( ntohs( address.sin_port ) );
    // The credential goes in the child's private environment, never its public argv.
    LPWCH inheritedEnvironment = GetEnvironmentStringsW();
    if ( !inheritedEnvironment )
        return E_OUTOFMEMORY;
    std::vector<std::wstring> entries;
    std::vector<WCHAR> environment;
    try
    {
        for ( const WCHAR *entry = inheritedEnvironment; *entry; entry += wcslen( entry ) + 1 )
        {
            if ( !_wcsnicmp( entry, L"NEXUS_NATIVE_TEXT_TOKEN=", 24 ) )
                continue;
            entries.emplace_back( entry );
        }
        entries.emplace_back( L"NEXUS_NATIVE_TEXT_TOKEN=" + encoded );
        std::sort( entries.begin(), entries.end(),
                   []( const auto &a, const auto &b ) { return _wcsicmp( a.c_str(), b.c_str() ) < 0; } );
        for ( auto &entry : entries )
        {
            environment.insert( environment.end(), entry.begin(), entry.end() );
            environment.push_back( 0 );
            SecureZeroMemory( entry.data(), entry.size() * sizeof( WCHAR ) );
        }
        environment.push_back( 0 );
        SecureZeroMemory( encoded.data(), encoded.size() * sizeof( WCHAR ) );
    }
    catch ( ... )
    {
        FreeEnvironmentStringsW( inheritedEnvironment );
        throw;
    }
    FreeEnvironmentStringsW( inheritedEnvironment );
    STARTUPINFOW startup{};
    startup.cb = sizeof( startup );
    PROCESS_INFORMATION process{};
    BOOL started = CreateProcessW( application.c_str(), command.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr,
                                   &startup, &process );
    DWORD error = started ? ERROR_SUCCESS : GetLastError();
    SecureZeroMemory( environment.data(), environment.size() * sizeof( WCHAR ) );
    if ( !started )
        return HRESULT_FROM_WIN32( error );
    CloseHandle( process.hThread );
    {
        TextEntryLock lock( &ctx->processMutex );
        ctx->process = process.hProcess;
        if ( ctx->cancelled.load() )
            TerminateProcess( ctx->process, ERROR_CANCELLED );
    }
    HRESULT result = E_FAIL;
    try
    {
        bool authorized = false;
        for ( int attempt = 0; attempt < 3 && !ctx->cancelled.load(); ++attempt )
        {
            fd_set ready;
            FD_ZERO( &ready );
            FD_SET( listener.value, &ready );
            timeval timeout{ 10, 0 };
            if ( select( 0, &ready, nullptr, nullptr, &timeout ) <= 0 )
                break;
            connection.value = accept( listener.value, nullptr, nullptr );
            if ( connection.value == INVALID_SOCKET )
                break;
            DWORD receiveTimeout = 2000;
            setsockopt( connection.value, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char *>( &receiveTimeout ), sizeof( receiveTimeout ) );
            unsigned char response[32];
            if ( text_entry_transfer( connection.value, response, sizeof( response ), false ) &&
                 !memcmp( token, response, sizeof( token ) ) )
            {
                authorized = true;
                break;
            }
            closesocket( connection.value );
            connection.value = INVALID_SOCKET;
        }
        if ( authorized )
        {
            DWORD receiveTimeout = 0;
            setsockopt( connection.value, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char *>( &receiveTimeout ), sizeof( receiveTimeout ) );
            {
                TextEntryLock lock( &ctx->processMutex );
                ctx->connection = connection.value;
                if ( ctx->cancelled.load() )
                    shutdown( connection.value, SD_BOTH );
            }
            UINT32 header[] = { 0x3345544e,
                                ctx->limit,
                                ctx->numeric ? 1u : 0u,
                                ctx->password ? 1u : 0u,
                                static_cast<UINT32>( ctx->title.size() ),
                                static_cast<UINT32>( ctx->description.size() ),
                                static_cast<UINT32>( ctx->initial.size() ),
                                0,
                                0 };
            bool sent = text_entry_transfer( connection.value, header, sizeof( header ), true );
            for ( const auto *field : fields )
                if ( sent )
                    sent = text_entry_transfer( connection.value, const_cast<WCHAR *>( field->data() ),
                                                static_cast<DWORD>( field->size() * sizeof( WCHAR ) ), true );
            UINT32 reply[3]{};
            if ( sent && text_entry_transfer( connection.value, reply, sizeof( reply ), false ) &&
                 reply[0] == 0x3345544e && reply[2] <= 8388609 )
            {
                result = static_cast<HRESULT>( reply[1] );
                if ( SUCCEEDED( result ) )
                {
                    if ( !reply[2] )
                        result = E_FAIL;
                    else
                    {
                        std::vector<char> value( reply[2] );
                        if ( !text_entry_transfer( connection.value, value.data(), reply[2], false ) ||
                             value.back() != 0 || strlen( value.data() ) != value.size() - 1 )
                            result = E_FAIL;
                        else
                            ctx->result.assign( value.data(), value.size() - 1 );
                    }
                }
            }
            {
                TextEntryLock lock( &ctx->processMutex );
                ctx->connection = INVALID_SOCKET;
            }
            shutdown( connection.value, SD_BOTH );
        }
    }
    catch ( ... )
    {
        result = E_OUTOFMEMORY;
    }
    {
        TextEntryLock lock( &ctx->processMutex );
        ctx->connection = INVALID_SOCKET;
        if ( WaitForSingleObject( process.hProcess, 3000 ) != WAIT_OBJECT_0 )
            TerminateProcess( process.hProcess, ERROR_CANCELLED );
        ctx->process = nullptr;
        CloseHandle( process.hProcess );
    }
    return result;
}
