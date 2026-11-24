// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/System/XUser.cpp"
static unsigned failures, checks;
#define CHECK( v )                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if ( !( v ) )                                                                                                  \
        {                                                                                                              \
            ++failures;                                                                                                \
            printf( "FAIL line %u\n", __LINE__ );                                                                      \
        }                                                                                                              \
    } while ( 0 )
class TitleTransport : public UserTransport
{
public:
    DWORD status = 200;
    std::string claim = "305419896";
    bool includeToken = true;
    unsigned requests = 0;
    HRESULT Backend( UINT16, const char *, UINT16, char **, UINT16 * ) override
    {
        return E_ACCESSDENIED;
    }
    HRESULT Http( const WCHAR *, const WCHAR *, char *, const WCHAR *, const WCHAR **, BYTE **, SIZE_T * ) override
    {
        return E_ACCESSDENIED;
    }
    HRESULT HttpRaw( const WCHAR *method, const WCHAR *url, const WCHAR *headers, const void *body, DWORD size,
                     DWORD *outStatus, BYTE **out, SIZE_T *length ) override
    {
        ++requests;
        CHECK( !wcscmp( method, L"POST" ) );
        CHECK( !wcscmp( url, L"https://title.auth.xboxlive.com/title/authenticate" ) );
        CHECK( wcsstr( headers, L"Signature: " ) && wcsstr( headers, L"x-xbl-contract-version: 1" ) );
        std::string sent( static_cast<const char *>( body ), size );
        CHECK( sent.find( "\"TitleId\":305419896" ) != std::string::npos );
        CHECK( sent.find( "\"DeviceToken\":\"synthetic-device\"" ) != std::string::npos );
        CHECK( sent.find( "\"ProofKey\":{" ) != std::string::npos );
        CHECK( sent.find( "synthetic-other-title" ) == std::string::npos );
        std::string response = "{\"DisplayClaims\":{\"xti\":{\"tid\":" + claim + "}}";
        if ( includeToken )
            response += ",\"Token\":\"synthetic-windows-title\"";
        response += "}";
        *outStatus = status;
        *length = response.size();
        *out = static_cast<BYTE *>( malloc( response.size() + 1 ) );
        memcpy( *out, response.c_str(), response.size() + 1 );
        return S_OK;
    }
};

class LegacyInitTransport final : public TitleTransport
{
public:
    bool sawTitleAuthorization = false, sawSignedDiscovery = false;
    static HRESULT Reply( const char *text, BYTE **out, SIZE_T *length )
    {
        *length = strlen( text );
        *out = static_cast<BYTE *>( malloc( *length + 1 ) );
        if ( !*out )
            return E_OUTOFMEMORY;
        memcpy( *out, text, *length + 1 );
        return S_OK;
    }
    HRESULT Backend( UINT16 type, const char *, UINT16, char **out, UINT16 *length ) override
    {
        CHECK( type == 3 );
        const char xml[] =
            "<MSATokenResponse><Token>synthetic-ticket</Token><Expiry>4102444800</Expiry>"
            "<DeviceRps>synthetic-device-ticket</DeviceRps><DeviceExpiry>4102444800</DeviceExpiry></MSATokenResponse>";
        *out = strdup( xml );
        *length = sizeof( xml ) - 1;
        return S_OK;
    }
    HRESULT Http( const WCHAR *method, const WCHAR *url, char *, const WCHAR *headers, const WCHAR **, BYTE **out,
                  SIZE_T *length ) override
    {
        if ( !wcscmp( url, L"https://device.auth.xboxlive.com/device/authenticate" ) )
            return Reply( R"({"Token":"synthetic-device"})", out, length );
        if ( !wcscmp( url, L"https://sisu.xboxlive.com/authorize" ) )
            return Reply(
                R"({"UserToken":{"Token":"synthetic-user"},"TitleToken":{"Token":"synthetic-other-title","DisplayClaims":{"xti":{"tid":19}}},"AuthorizationToken":{"Token":"synthetic-platform-auth","DisplayClaims":{"xui":[{"xid":"123456789","gtg":"Synthetic","mgt":"Synthetic","mgs":"","umg":"Synthetic","agg":"Adult"}]}}})",
                out, length );
        if ( !wcscmp( url, L"https://title.mgt.xboxlive.com/titles/current/endpoints" ) )
        {
            CHECK( !wcscmp( method, L"GET" ) );
            CHECK( headers && wcsstr( headers, L"Authorization: XBL3.0 x=synthetic-hash;synthetic-title-xsts" ) );
            CHECK( headers && wcsstr( headers, L"Signature: " ) );
            sawSignedDiscovery = true;
        }
        else
            CHECK( !wcscmp( url, L"https://title.mgt.xboxlive.com/titles/default/endpoints?type=1" ) );
        return Reply( R"({"SignaturePolicies":[],"EndPoints":[]})", out, length );
    }
    HRESULT HttpRaw( const WCHAR *method, const WCHAR *url, const WCHAR *headers, const void *body, DWORD size,
                     DWORD *outStatus, BYTE **out, SIZE_T *length ) override
    {
        if ( !wcscmp( url, L"https://title.auth.xboxlive.com/title/authenticate" ) )
            return TitleTransport::HttpRaw( method, url, headers, body, size, outStatus, out, length );
        CHECK( !wcscmp( url, L"https://xsts.auth.xboxlive.com/xsts/authorize" ) );
        std::string request( static_cast<const char *>( body ), size );
        CHECK( request.find( "\"TitleToken\":\"synthetic-windows-title\"" ) != std::string::npos );
        CHECK( request.find( "synthetic-other-title" ) == std::string::npos );
        sawTitleAuthorization = true;
        *outStatus = 200;
        return Reply(
            R"({"Token":"synthetic-title-xsts","DisplayClaims":{"xui":[{"uhs":"synthetic-hash","xid":"123456789"}]}})",
            out, length );
    }
};

int main()
{
    InitializeLogging();
    CoInitializeEx( nullptr, COINIT_MULTITHREADED );
    const char xml[] = "<Game configVersion='0'><Identity Name='Synthetic.Legacy' "
                       "Publisher='CN=Test'/><TitleId>12345678</TitleId></Game>";
    INITIALIZE_OPTIONS options{ 1, TRUE, xml };
    CHECK( nexus::InitializeGameConfiguration( &options ) == S_OK );
    for ( unsigned mode = 0; mode < 5; ++mode )
    {
        TitleTransport transport;
        XUser user( transport );
        CHECK( user.Initialize( XUserAddOptions::AddDefaultUserSilently ) == S_OK );
        user.deviceToken = strdup( "synthetic-device" );
        if ( mode == 1 )
            transport.claim = "305419897";
        if ( mode == 2 )
            transport.claim = "\"0305419896\"";
        if ( mode == 3 )
            transport.status = 403;
        if ( mode == 4 )
            transport.includeToken = false;
        HRESULT result = windows_title_auth( &user );
        CHECK( transport.requests == 1 );
        if ( mode == 0 )
        {
            CHECK( result == S_OK );
            CHECK( user.titleToken && !strcmp( user.titleToken, "synthetic-windows-title" ) );
            CHECK( user.titleTokenTitleId == 0x12345678 );
        }
        else
        {
            CHECK( FAILED( result ) );
            CHECK( !user.titleToken );
            CHECK( !user.titleTokenTitleId );
        }
        user.ResetNetworkState();
    }
    {
        LegacyInitTransport transport;
        XUser user( transport );
        CHECK( user.Initialize( XUserAddOptions::AddDefaultUserSilently ) == S_OK );
        CHECK( user.InitializeNetwork( XUserAddOptions::AddDefaultUserSilently ) == S_OK );
        CHECK( transport.requests == 1 && transport.sawTitleAuthorization && transport.sawSignedDiscovery );
        CHECK( user.titleTokenTitleId == 0x12345678 );
        user.ResetNetworkState();
    }
    {
        TitleTransport transport;
        XUser user( transport );
        user.cancelled = true;
        CHECK( windows_title_auth( &user ) == E_ABORT );
        CHECK( transport.requests == 0 );
    }
    CoUninitialize();
    printf( "CHECKS %u FAILURES %u\n", checks, failures );
    return failures ? 1 : 0;
}
