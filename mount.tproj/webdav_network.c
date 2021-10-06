/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include "webdavd.h"

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCDynamicStorePrivate.h>
#include <SystemConfiguration/SCDynamicStoreKey.h>
#include <CoreServices/CoreServices.h>
#include <CoreServices/CoreServicesPriv.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <Security/Security.h>

#include "webdav_parse.h"
#include "webdav_requestqueue.h"
#include "webdav_authcache.h"
#include "webdav_network.h"
#include "EncodedSourceID.h"

/******************************************************************************/

/*
 * BODY_BUFFER_SIZE is the initial size of the buffer used to read an
 * HTTP entity body. The largest bodies are typically the XML data
 * returned by the PROPFIND method for a large collection (directory).
 * 64K is large enough to handle directories with 100-150 items.
 */
#define BODY_BUFFER_SIZE 0x10000	/* 64K */

#define kSSLClientPropTLSServerCertificateChain CFSTR("TLSServerCertificateChain") /* array[data] */
#define kSSLClientPropTLSTrustClientStatus	CFSTR("TLSTrustClientStatus") /* CFNumberRef of kCFNumberSInt32Type (errSSLxxxx) */
#define kSSLClientPropTLSServerHostName	CFSTR("TLSServerHostName") /* CFString */

struct HeaderFieldValue
{
	CFStringRef	headerField;
	CFStringRef	value;
};

/******************************************************************************/

static CFStringRef userAgentHeaderValue = NULL;	/* The User-Agent request-header value */
static CFIndex first_read_len = 4096;	/* bytes.  Amount to download at open so first read at offset 0 doesn't stall */
static CFStringRef X_Source_Id_HeaderValue = NULL;	/* the X-Source-Id header value, or NULL if not iDisk */

static SCDynamicStoreRef gProxyStore;

/******************************************************************************/

static pthread_mutex_t gNetworkGlobals_lock;
/* these variables are protected by gNetworkGlobals_lock */
static CFDictionaryRef gProxyDict = NULL;
static int gHttpProxyEnabled;
static char gHttpProxyServer[MAXHOSTNAMELEN];
static int gHttpProxyPort;
static int gHttpsProxyEnabled;
static char gHttpsProxyServer[MAXHOSTNAMELEN];
static int gHttpsProxyPort;
static CFMutableDictionaryRef gSSLPropertiesDict = NULL;
static struct ReadStreamRec gReadStreams[WEBDAV_REQUEST_THREADS + 1];	/* one for every request thread plus one for the pulse thread */

/******************************************************************************/

static int network_stat(
	uid_t uid,					/* -> uid of the user making the request */
	CFURLRef urlRef,			/* -> url to the resource */
	struct stat *statbuf);		/* <- stat information is returned in this buffer */

static int network_dir_is_empty(
	uid_t uid,					/* -> uid of the user making the request */
	CFURLRef urlRef);			/* -> url to check */

static time_t DateStringToTime(	/* <- time_t value; -1 if error */
	CFStringRef str);			/* -> CFString to parse */

static CFStringRef CFStringCreateRFC2616DateStringWithTimeT( /* <- CFString containing RFC 1123 date, NULL if error */
	time_t clock);				/* -> time_t value */

/*****************************************************************************/

/*
 * DateBytesToTime parses the RFC 850, RFC 1123, and asctime formatted
 * date/time bytes and returns time_t. If the parse fails, this function
 * returns -1.
 */
time_t DateBytesToTime(			/* <- time_t value */
	const UInt8 *bytes,			/* -> pointer to bytes to parse */
	CFIndex length)				/* -> number of bytes to parse */
{
	const UInt8 *finish;
	CFGregorianDate gdate;
	struct tm tm_temp;
	time_t clock;
	
	/* parse the RFC 850, RFC 1123, and asctime formatted date/time CFString to get the Gregorian date */
	finish = _CFGregorianDateCreateWithBytes(kCFAllocatorDefault, bytes, length, &gdate, NULL);
	require_action(finish != bytes, _CFGregorianDateCreateWithBytes, clock = -1);
	
	/* copy the Gregorian date into struct tm */
	memset(&tm_temp, 0, sizeof(struct tm));
	tm_temp.tm_sec = gdate.second;
	tm_temp.tm_min = gdate.minute;
	tm_temp.tm_hour = gdate.hour;
	tm_temp.tm_mday = gdate.day;
	tm_temp.tm_mon = gdate.month - 1;
	tm_temp.tm_year = gdate.year - 1900;

	clock = timegm(&tm_temp);

_CFGregorianDateCreateWithBytes:

	return ( clock );
}

/*****************************************************************************/

/*
 * DateStringToTime parses the RFC 850, RFC 1123, and asctime formatted
 * date/time CFString and returns time_t. If the parse fails, this function
 * returns -1.
 */
static time_t DateStringToTime(	/* <- time_t value; -1 if error */
	CFStringRef str)			/* -> CFString to parse */
{
	CFIndex count;
	CFGregorianDate gdate;
	struct tm tm_temp;
	time_t clock;
	
	/* parse the RFC 850, RFC 1123, and asctime formatted date/time CFString to get the Gregorian date */
	count = _CFGregorianDateCreateWithString(kCFAllocatorDefault, str, &gdate, NULL);
	require_action(count != 0, _CFGregorianDateCreateWithString, clock = -1);
	
	/* copy the Gregorian date into struct tm */
	memset(&tm_temp, 0, sizeof(struct tm));
	tm_temp.tm_sec = gdate.second;
	tm_temp.tm_min = gdate.minute;
	tm_temp.tm_hour = gdate.hour;
	tm_temp.tm_mday = gdate.day;
	tm_temp.tm_mon = gdate.month - 1;
	tm_temp.tm_year = gdate.year - 1900;

	clock = timegm(&tm_temp);

_CFGregorianDateCreateWithString:

	return ( clock );
}

/*****************************************************************************/

/*
 * CFStringCreateRFC2616DateStringWithTimeT creates a RFC 1123 date CFString from
 * a time_t time.
 */
static CFStringRef CFStringCreateRFC2616DateStringWithTimeT( /* <- CFString containing RFC 1123 date, NULL if error */
	time_t clock)				/* -> time_t value */
{
	struct tm *tm_temp;
	CFGregorianDate gdate;
	CFStringRef result;
	
	require_action_quiet(clock != -1, InvalidClock, result = NULL);
	
	/* get struct tm (in GMT) from time_t */
	tm_temp = gmtime(&clock);

	/* copy the struct tm into CFGregorianDate */
	gdate.second = tm_temp->tm_sec;
	gdate.minute = tm_temp->tm_min;
	gdate.hour = tm_temp->tm_hour;
	gdate.day = tm_temp->tm_mday;
	gdate.month = tm_temp->tm_mon + 1;
	gdate.year = tm_temp->tm_year + 1900;
	
	/* get a CFStringRef with the RFC 1123 date string */
	result = _CFStringCreateRFC2616DateStringWithGregorianDate(kCFAllocatorDefault, &gdate, NULL);

InvalidClock:
	
	return ( result );
}

/*****************************************************************************/

/*
 * SkipCodedURL finds the end of a Coded-URL using the rules
 * (rfc 2518, section 9.4 and rfc 2396):
 *	
 *	Coded-URL		= "<" absoluteURI ">"
 *	
 * On input, the bytes parameter points to the character AFTER the initial
 * '<' character. The function result is a pointer to the '>' character that
 * terminates the Coded-URL or the end of the C string.
 */
static const char * SkipCodedURL(const char *bytes)
{
	/* the end of the string or Coded-URL? */
	while ( (*bytes != '\0') && (*bytes != '>') )
	{
		/* skip character */
		++bytes;
	}
	
	return ( bytes );
}

/*****************************************************************************/

/*
 * SkipToken finds the end of a token using the rules (rfc 2616, section 2.2):
 *	
 *	token		= 1*<any CHAR except CTLs or separators>
 *	CTL			= <any US-ASCII control character (octets 0 - 31) and
 *				  DEL (127)>
 *	separators	= "(" | ")" | "<" | ">" | "@"
 *				  | "," | ";" | ":" | "\" | <">
 *				  | "/" | "[" | "]" | "?" | "="
 *				  | "{" | "}" | SP | HT
 *	
 * The function result is a pointer to the first non token character or the
 * end of the C string.
 */
static const char * SkipToken(const char *bytes)
{
	while ( *bytes != '\0' )
	{
		/* CTL - US-ASCII control character (octets 0 - 31) */
		if ( (unsigned char)*bytes <= 31 )
		{
			/* not a token character - done */
			goto Done;
		}
		else
		{
			switch ( *bytes )
			{
			/* CTL - DEL (127) */
			case '\x7f':
			/* separators */
			case '(':
			case ')':
			case '<':
			case '>':
			case '@':
			case ',':
			case ';':
			case ':':
			case '\\':
			case '\"':
			case '/':
			case '[':
			case ']':
			case '\?':
			case '=':
			case '{':
			case '}':
			case ' ':
			case '\t':
				/* not a token character - done */
				goto Done;
				break;
			
			default:
				/* skip token characters */
				++bytes;
				break;
			}
		}
	}

Done:	
	return (bytes);
}

/*****************************************************************************/

/*
 * SkipLWS finds the end of a run of LWS using the rule
 * (rfc 2616, section 2.2):
 *	
 *	LWS = [CRLF] 1*( SP | HT )
 *	
 * The function result is a pointer to the first non LWS character or the end
 * of the C string.
 */
static const char * SkipLWS(const char *bytes)
{
	while ( *bytes != '\0' )
	{
		if ( (*bytes == ' ') || (*bytes == '\t') )
		{
			/* skip SP and HT characters */
			++bytes;
			continue;
		}
		/*
		 * skip CRLF only if followed by SP or HT (in which case the SP 
		 * or HT can be skipped, too)
		 */
		else if ( *bytes == '\x0d' ) /* CR? */
		{
			/* LF? */
			if ( bytes[1] == '\x0a' )
			{
				/* SP or HT? */
				if ( (bytes[2] == ' ') || (bytes[2] == '\t') )
				{	
					/* skip CRLF followed by SP or HT */
					bytes += 3;
					continue;
				}
			}
		}
		
		/* found the end of the LWS run */
		break;
	}
	
	return ( bytes );
}

/*****************************************************************************/

static int set_global_stream_properties(CFReadStreamRef readStreamRef)
{
	int error, mutexerror;
	
	error = 0;
	
	mutexerror = pthread_mutex_lock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_lock, error = mutexerror; webdav_kill(-1));
	
	error = (CFReadStreamSetProperty(readStreamRef, kCFStreamPropertyHTTPProxy, gProxyDict) == TRUE) ? 0 : 1;
	
	mutexerror = pthread_mutex_unlock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_unlock, error = mutexerror; webdav_kill(-1));

pthread_mutex_unlock:
pthread_mutex_lock:
	
	return ( error );
}

/******************************************************************************/

int network_get_proxy_settings(
	int *httpProxyEnabled,		/* <- true if HTTP proxy is enabled */
	char **httpProxyServer,		/* <- HTTP proxy server name */
	int *httpProxyPort,			/* <- HTTP proxy port number */
	int *httpsProxyEnabled,		/* <- true if HTTPS proxy is enabled */
	char **httpsProxyServer,	/* <- HTTPS proxy server name */
	int* httpsProxyPort)		/* <- HTTPS proxy port number */
{
	int error, mutexerror;
	
	error = 0;
	
	mutexerror = pthread_mutex_lock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_lock, error = mutexerror; webdav_kill(-1));
	
	*httpProxyServer = malloc(MAXHOSTNAMELEN);
	require_action(*httpProxyServer != NULL, malloc_httpProxyServer, error = ENOMEM);
	
	*httpsProxyServer = malloc(MAXHOSTNAMELEN);
	require_action(*httpsProxyServer != NULL, malloc_httpsProxyServer, free(*httpProxyServer); error = ENOMEM);
	
	*httpProxyEnabled = gHttpProxyEnabled;
	memcpy(*httpProxyServer, gHttpProxyServer, MAXHOSTNAMELEN);
	*httpProxyPort = gHttpProxyPort;
	
	*httpsProxyEnabled = gHttpsProxyEnabled;
	memcpy(*httpsProxyServer, gHttpsProxyServer, MAXHOSTNAMELEN);
	*httpsProxyPort = gHttpsProxyPort;

malloc_httpsProxyServer:
malloc_httpProxyServer:
	
	mutexerror = pthread_mutex_unlock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_unlock, error = mutexerror; webdav_kill(-1));
	
pthread_mutex_unlock:
pthread_mutex_lock:
	
	return ( error );
}

/******************************************************************************/

int network_update_proxy(void *arg)
{
#pragma unused(arg)
	int error, mutexerror;
	
	error = 0;
	
	mutexerror = pthread_mutex_lock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_lock, error = mutexerror; webdav_kill(-1));

	/* release the old proxies dictionary */
	if ( gProxyDict )
	{
		CFRelease(gProxyDict);
	}
	
	/* slam everything to default disabled state in case something fails */
	gHttpProxyEnabled = 0;
	gHttpProxyServer[0] = '\0';
	gHttpProxyPort = 0;
	
	gHttpsProxyEnabled = 0;
	gHttpsProxyServer[0] = '\0';
	gHttpsProxyPort = 0;
	
	/* get the current internet proxy dictionary */
	gProxyDict = SCDynamicStoreCopyProxies(gProxyStore);
	if ( gProxyDict != NULL )
	{
		CFNumberRef cf_enabled;
		int enabled;
		CFStringRef cf_host;
		CFNumberRef cf_port;
		
		/*
		 * take care of HTTP proxies
		 */
		/* are HTTP proxies enabled? */ 
		cf_enabled = CFDictionaryGetValue(gProxyDict, kSCPropNetProxiesHTTPEnable);
		if ( (cf_enabled != NULL) && CFNumberGetValue(cf_enabled, kCFNumberIntType, &enabled) && enabled )
		{
			/* get the HTTP proxy */
			cf_host = CFDictionaryGetValue(gProxyDict, kSCPropNetProxiesHTTPProxy);
			if ( (cf_host != NULL) && CFStringGetCString(cf_host, gHttpProxyServer, sizeof(gHttpProxyServer), kCFStringEncodingUTF8) )
			{
				/* get the HTTP proxy port */
				cf_port = CFDictionaryGetValue(gProxyDict, kSCPropNetProxiesHTTPPort);
				if ( (cf_port != NULL) && CFNumberGetValue(cf_port, kCFNumberIntType, &gHttpProxyPort) ) 
				{
					if ( gHttpProxyPort == 0 )
					{
						/* no port specified so use the default HTTP port */
						gHttpProxyPort = kHttpDefaultPort;
					}
					gHttpProxyEnabled = 1;
				}
			}
		}
		
		/*
		 * take care of HTTPS proxies
		 */
		/* are HTTP proxies enabled? */ 
		cf_enabled = CFDictionaryGetValue(gProxyDict, kSCPropNetProxiesHTTPSEnable);
		if ( (cf_enabled != NULL) && CFNumberGetValue(cf_enabled, kCFNumberIntType, &enabled) && enabled )
		{
			/* get the HTTP proxy */
			cf_host = CFDictionaryGetValue(gProxyDict, kSCPropNetProxiesHTTPSProxy);
			if ( (cf_host != NULL) && CFStringGetCString(cf_host, gHttpsProxyServer, sizeof(gHttpsProxyServer), kCFStringEncodingUTF8) )
			{
				/* get the HTTP proxy port */
				cf_port = CFDictionaryGetValue(gProxyDict, kSCPropNetProxiesHTTPSPort);
				if ( (cf_port != NULL) && CFNumberGetValue(cf_port, kCFNumberIntType, &gHttpsProxyPort) ) 
				{
					if ( gHttpsProxyPort == 0 )
					{
						/* no port specified so use the default HTTPS port */
						gHttpsProxyPort = kHttpsDefaultPort;
					}
					gHttpsProxyEnabled = 1;
				}
			}
		}
	}
	
	mutexerror = pthread_mutex_unlock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_unlock, error = mutexerror; webdav_kill(-1));
	
	/* remove proxy authentications */
	error = authcache_proxy_invalidate();

pthread_mutex_unlock:
pthread_mutex_lock:

	return ( error );
}

/*****************************************************************************/

/*
	The InitUserAgentHeaderValue initializes the string userAgentHeaderValue which
	is sent with every request to the server. The User-Agent request-header field
	is defined in RFC 2616, section 14.43 as:
		User-Agent		= "User-Agent" ":" 1*( product | comment )
	section 3.8 defines product as:
		product			= token ["/" product-version]
		product-version	= token
	section 2.2 defines comment as:
		comment			= "(" *( ctext | quoted-pair | comment ) ")"
		ctext			= <any TEXT excluding "(" and ")">
		quoted-pair		= "\" CHAR

	We want our User-Agent request-header field to look something like:
		"User-Agent: WebDAVFS/1.1 (0110800000) Darwin/5.3 (Power Macintosh)"
	where:
		1.1	= the CFBundleShortVersionString from webdav.fs bundle
		0110800000 = webdav.fs bundle's numeric version
		Darwin = CTL_KERN/KERN_OSTYPE
		5.3 = CTL_KERN/KERN_OSRELEASE
		Power Macintosh = CTL_HW/HW_MACHINE
	
	webdav.fs bundle is located at:
		/System/Library/Filesystems/webdav.fs
	
	If the data from webdav.fs bundle could not be obtained, then we'll
	fall back to the generic User-Agent request-header string WebDAV FS
	used to use.
	
	IMPORTANT: The user-agent header MUST start with the
	product token "WebDAVFS" because there are WebDAV servers
	that special case this client.
 */

static int InitUserAgentHeaderValue(int add_mirror_comment)
{
	char				buf[128];
	int					mib[2];
	char				ostype[128];
	char				osrelease[128];
	char				machine[128];
	size_t				len;
	CFURLRef			url;
	CFBundleRef			bundle;
	CFDictionaryRef		dict;
	CFStringRef			shortVersion;
	CFIndex				shortVersionLen;
	char				*webdavfsVersionStr;
	UInt32				webdavfsVersion;
	int					result;
	
	/* Get the ostype, osrelease, and machine strings using sysctl*/
	mib[0] = CTL_KERN;
	mib[1] = KERN_OSTYPE;
	len = sizeof ostype;
	if (sysctl(mib, 2, ostype, &len, 0, 0) < 0)
	{
		ostype[0] = '\0';
	}
	mib[1] = KERN_OSRELEASE;
	len = sizeof osrelease;
	if (sysctl(mib, 2, osrelease, &len, 0, 0) < 0)
	{
		osrelease[0] = '\0';
	}
	mib[0] = CTL_HW;
	mib[1] = HW_MACHINE;
	len = sizeof machine;
	if (sysctl(mib, 2, machine, &len, 0, 0) < 0)
	{
		machine[0] = '\0';
	}
	
	/* We don't have it yet */
	webdavfsVersionStr = NULL;
	webdavfsVersion = 0x010080000; /* 1.0 final */
	
	/* Create the CFURLRef to the webdav.fs bundle's version.plist */
	url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
		CFSTR("/System/Library/Filesystems/webdav.fs"),
		kCFURLPOSIXPathStyle, true);
	if ( url != NULL )
	{
		/* Create the bundle */
		bundle = CFBundleCreate(kCFAllocatorDefault, url);
		if ( bundle != NULL )
		{
			/* Get the bundle's numeric version */
			webdavfsVersion = CFBundleGetVersionNumber(bundle);
			
			/* Get the Info dictionary */ 
			dict = CFBundleGetInfoDictionary(bundle);
			if ( dict != NULL )
			{
				/* Get the CFBundleShortVersionString (display string) */
				shortVersion = CFDictionaryGetValue(dict, CFSTR("CFBundleShortVersionString"));
				if ( shortVersion != NULL )
				{
					/* Get the bundleVersionStr */
					shortVersionLen = CFStringGetLength(shortVersion) + 1;
					webdavfsVersionStr = malloc((size_t)shortVersionLen);
					if ( webdavfsVersionStr != NULL )
					{
						/* Convert it to a C string */
						if ( !CFStringGetCString(shortVersion, webdavfsVersionStr, shortVersionLen, kCFStringEncodingUTF8) )
						{
							/* If we can't get it, free the memory */
							free(webdavfsVersionStr);
							webdavfsVersionStr = NULL;
						}
					}
				}
			}
			/* release created bundle */
			CFRelease(bundle);
		}
		/* release created url */
		CFRelease(url);
	}
	
	if ( webdavfsVersionStr != NULL )
	{
		/* if everything worked, use the new format User-Agent request-header string */
		snprintf(buf, sizeof(buf), "WebDAVFS/%s (%.8lx) %s%s/%s (%s)",
			webdavfsVersionStr, webdavfsVersion, (add_mirror_comment ? "(mirrored) " : ""), ostype, osrelease, machine);
		free(webdavfsVersionStr);
	}
	else
	{
		/* create the generic User-Agent request-header string WebDAV FS used to use */
		snprintf(buf, sizeof(buf), "WebDAVFS/1.4 %s/%s (%s)",
			ostype, osrelease, machine);
	}
			
	userAgentHeaderValue = CFStringCreateWithCString(kCFAllocatorDefault, buf, kCFStringEncodingUTF8);
	require_action(userAgentHeaderValue != NULL, CFStringCreateWithCString, result = ENOMEM);
	
	result = 0;

CFStringCreateWithCString:
	
	return ( result );
}

/*****************************************************************************/

/*
 * The get_first_read_len function sets the global
 * first_read_len. It is set to the system's page size so that if the
 * first read after an open starts at offset 0, that page will already be
 * downloaded into the cache file.
 */
static void get_first_read_len(void)
{
	int		mib[2];
	size_t	len;
	int		result;
	int		pagesize;
	
	/* get the hardware page size */
	mib[0] = CTL_HW;
	mib[1] = HW_PAGESIZE;
	len = sizeof(int);
	result = sysctl(mib, 2, &pagesize, &len, 0, 0);
	if ( result < 0 )
	{
		/* set first_read_len to PowerPC page size */
		first_read_len = 4096;
	}
	else
	{
		first_read_len = pagesize;
	}
}

/******************************************************************************/

static void InitXSourceIdHeaderValue(void)
{
	CFStringRef hostName;
	char encodedIdBuffer[32];
	
	X_Source_Id_HeaderValue = NULL;
	
	/* get the host name */
	hostName = CFURLCopyHostName(gBaseURL);
	require_quiet(hostName != NULL, CFURLCopyHostName);
	
	/* is it "idisk.mac.com" - if not, we don't want it */
	require_quiet(CFStringCompare(hostName, CFSTR("idisk.mac.com"), kCFCompareCaseInsensitive) == kCFCompareEqualTo, NotIdisk);
	
	/* created the X-Source-Id string */
	require_quiet(GetEncodedSourceID(encodedIdBuffer), GetEncodedSourceID);
	
	/* and make it a CFString */
	X_Source_Id_HeaderValue = CFStringCreateWithCString(kCFAllocatorDefault, encodedIdBuffer, kCFStringEncodingUTF8);

GetEncodedSourceID:
NotIdisk:
	CFRelease(hostName);
CFURLCopyHostName:

	return;
}

/******************************************************************************/

int network_init(const UInt8 *uri, CFIndex uriLength, int *store_notify_fd, int add_mirror_comment)
{
	int error;
	pthread_mutexattr_t mutexattr;
	CFStringRef notification_string;
	CFArrayRef keys;
	int index;
	
	gProxyDict = NULL;
	error = 0;
	
	/* set up the lock on the queues */
	error = pthread_mutexattr_init(&mutexattr);
	require_noerr(error, pthread_mutexattr_init);
	
	error = pthread_mutex_init(&gNetworkGlobals_lock, &mutexattr);
	require_noerr(error, pthread_mutex_init);
	
	/* create a dynnamic store */
	gProxyStore = SCDynamicStoreCreate(kCFAllocatorDefault, CFSTR("WebDAVFS"), NULL, NULL);
	require_action(gProxyStore != NULL, SCDynamicStoreCreate, error = ENOMEM);
	
	/* open a file descriptor to be notified on */
	require_action(SCDynamicStoreNotifyFileDescriptor(gProxyStore, 0, store_notify_fd),
		SCDynamicStoreNotifyFileDescriptor, error = ENOMEM);

	/* create a keys for network proxy changes */
	notification_string = SCDynamicStoreKeyCreateProxies(kCFAllocatorDefault);
	require_action(notification_string != NULL, SCDynamicStoreKeyCreateProxies, error = ENOMEM);
	
	keys = CFArrayCreate(kCFAllocatorDefault, (const void **)&notification_string, 1, &kCFTypeArrayCallBacks);
	require_action(keys != NULL, CFArrayCreate, error = ENOMEM);
	CFRelease(notification_string);
	
	require_action(SCDynamicStoreSetNotificationKeys(gProxyStore, keys, NULL),
		SCDynamicStoreSetNotificationKeys, error = ENOMEM);
	CFRelease(keys);

	/* get the initial internet proxy settings */
	error = network_update_proxy(NULL);
	require_noerr_quiet(error, network_update_proxy);

	/* create gBaseURL */
	gBaseURL = CFURLCreateAbsoluteURLWithBytes(kCFAllocatorDefault, uri, uriLength, kCFStringEncodingUTF8, NULL, FALSE);
	require_action_string(gBaseURL != NULL, CFURLCreateAbsoluteURLWithBytes, error = ENOMEM, "name was not legal UTF8");

	/* initialize first_read_len variable */
	get_first_read_len();
	
	/* init the X_Source_Id_HeaderValue string BEFORE InitUserAgentHeaderValue */
	InitXSourceIdHeaderValue();

	/* initialize userAgentHeaderValue */
	error = InitUserAgentHeaderValue((X_Source_Id_HeaderValue != NULL) && add_mirror_comment);
	if ( error )
	{
		/* not likely to fail, but just in case */
		exit(error);
	}
	
	/* initialize the gReadStreams array */
	for ( index = 0; index < (WEBDAV_REQUEST_THREADS + 1); ++index )
	{
		gReadStreams[index].inUse = 0; /* not in use */
		gReadStreams[index].readStreamRef = NULL; /* no stream */
		gReadStreams[index].uniqueValue = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%d"), index); /* unique string */
	}

CFURLCreateAbsoluteURLWithBytes:
network_update_proxy:
SCDynamicStoreSetNotificationKeys:
CFArrayCreate:
SCDynamicStoreKeyCreateProxies:
SCDynamicStoreNotifyFileDescriptor:
SCDynamicStoreCreate:
pthread_mutex_init:
pthread_mutexattr_init:
	
	return ( error );
}

/******************************************************************************/

/*
 * create_cfurl_from_node
 *
 * Creates a CFURL to the node if no name is provided, or to the node's named
 * child if a name is provided.
 *
 * The caller is responsible for releasing the CFURL returned.
 */
static CFURLRef create_cfurl_from_node(
	struct node_entry *node,
	char *name,
	size_t name_length)
{
	CFURLRef tempUrlRef;
	CFURLRef urlRef;
	char *node_path;
	int error;
	
	urlRef = NULL;
	
	/*
	 * Get the UT8 path (not percent escaped) from the root to the node (if any).
	 * If the path is returned and it is to a directory, it will end with a slash.
	 */
	error = nodecache_get_path_from_node(node, &node_path);
	require_noerr_quiet(error, nodecache_get_path_from_node);
	
	/* append the name (if any) */
	if ( name != NULL && name_length != 0 )
	{
		strncat(node_path, name, name_length);
	}
	
	/* is there any relative path? */
	if ( *node_path != '\0' )
	{
		CFStringRef stringRef;
		CFStringRef	escapedPathRef;
		
		/* convert the relative path to a CFString */
		stringRef = CFStringCreateWithCString(kCFAllocatorDefault, node_path, kCFStringEncodingUTF8);
		require_string(stringRef != NULL, CFStringCreateWithCString, "name was not legal UTF8");
		
		/*
		 * Then percent escape everything that CFURLCreateStringByAddingPercentEscapes()
		 * considers illegal URL characters plus the characters ";" and "?" which are
		 * not legal pchar (see rfc2396) characters, and ":" so that names in the root
		 * directory do not look like absolute URLs with some weird scheme.
		 */
		escapedPathRef = CFURLCreateStringByAddingPercentEscapes(kCFAllocatorDefault, stringRef, NULL, CFSTR(":;?"), kCFStringEncodingUTF8);
		require(escapedPathRef != NULL, CFURLCreateStringByAddingPercentEscapes);
		
		/* create the relative URL */
		urlRef = CFURLCreateWithString(kCFAllocatorDefault, escapedPathRef, gBaseURL);
		require(urlRef != NULL, CFURLCreateWithString);
		
		/* and then make an absolute copy of it */
		tempUrlRef = urlRef;
		urlRef = CFURLCopyAbsoluteURL(tempUrlRef);
		
		CFRelease(tempUrlRef);
CFURLCreateWithString:
		CFRelease(escapedPathRef);
CFURLCreateStringByAddingPercentEscapes:	
		CFRelease(stringRef);
CFStringCreateWithCString:
		;
	}
	else
	{
		/* no relative path -- use the base URL */
		CFRetain(gBaseURL);
		urlRef = gBaseURL;
	}

	free(node_path);

nodecache_get_path_from_node:
		
	return ( urlRef );
}

/******************************************************************************/

static int translate_status_to_error(UInt32 statusCode)
{
	int result;
	
	/* switch by Nxx range */
	switch ( statusCode / 100 )
	{
		case 1:	/* Informational 1xx */
			syslog(LOG_ERR,"unexpected statusCode %ld", statusCode);
			result = ENOENT;	/* CFNetwork eats 1xx responses so this should never happen */
			break;
		case 2:	/* Successful 2xx */
			result = 0;
			break;
		case 3:	/* Redirection 3xx */
			syslog(LOG_ERR,"unexpected statusCode %ld", statusCode);
			result = ENOENT;
			break;
		case 4:	/* Client Error 4xx */
			switch ( statusCode )
			{
				case 401:	/* 401 Unauthorized */
				case 407:	/* 407 Proxy Authentication Required */
					result = EAUTH;
					break;
				case 402:	/* Payment required */
				case 403:	/* Forbidden */
					result = EPERM;
					break;
				case 404:	/* Not Found */
				case 409:	/* Conflict (path prefix does not exist) */
				case 410:	/* Gone */
					result = ENOENT;
					break;
				case 414:	/* Request URI Too Long */
					result = ENAMETOOLONG;
				case 423:	/* Locked (WebDAV) */
				case 424:	/* Failed Dependency (WebDAV) (EBUSY when a directory cannot be MOVE'd) */
					result = EBUSY;
					break;
				default:
					syslog(LOG_ERR,"unexpected statusCode %ld", statusCode);
					result = EINVAL;	/* unexpected */
					break;
			}
			break;
		case 5:	/* Server Error 5xx */
			if ( statusCode == 507 )	/* Insufficient Storage (WebDAV) */
			{
				result = ENOSPC;
			}
			else
			{
				syslog(LOG_ERR,"unexpected statusCode %ld", statusCode);
				result = ENOENT;	/* unexpected */
			}
			break;
		default: /* only the 1xx through 5xx ranges are defined */
			syslog(LOG_ERR,"unexpected statusCode %ld", statusCode);
			result = EIO;
			break;
	}
	
	return ( result );
}

/******************************************************************************/

/* returns TRUE if SSL properties were correctly applied (or were not needed) */
static int ApplySSLProperties(CFReadStreamRef readStreamRef)
{
	int result = TRUE;
	
	if ( gSSLPropertiesDict != NULL ) 
	{
		result = (CFReadStreamSetProperty(readStreamRef, kCFStreamPropertySSLSettings, gSSLPropertiesDict) == TRUE);
	}
	
	return ( result );
}

/******************************************************************************/

/*
 * get_ReadStreamRec
 *
 * Tries to return a ReadStreamRec that's not in use and open. If that's not
 * possible, returns the first ReadStreamRec that's not in use and closed.
 */
static struct ReadStreamRec *get_ReadStreamRec(void)
{
	int index;
	struct ReadStreamRec *result;
	int mutexerror;
	
	result = NULL;
	
	/* grab gNetworkGlobals_lock */
	mutexerror = pthread_mutex_lock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_lock, webdav_kill(-1));
	
	for ( index = 0; index < (WEBDAV_REQUEST_THREADS + 1); ++index )
	{
		if ( !gReadStreams[index].inUse )
		{
			/* found one not in use */
			
			/* if it still looks open, grab it */
			if ( gReadStreams[index].readStreamRef != NULL )
			{
				result = &gReadStreams[index]; /* get pointer to it */
				result->inUse = TRUE;	/* mark it in use */
				break;
			}
			else if ( result == NULL )
			{
				/* else... keep track of the first closed one in case we don't find an open one */
				result = &gReadStreams[index];
			}
		}
	}
	
	if ( result != NULL )
	{
		/* should always happen */
		result->inUse = TRUE;	/* mark it in use */
	}

	/* release gNetworkGlobals_lock */
	mutexerror = pthread_mutex_unlock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_unlock, webdav_kill(-1));

pthread_mutex_unlock:
pthread_mutex_lock:

	return ( result );
}

/******************************************************************************/

/*
 * release_ReadStreamRec
 *
 * Release a ReadStreamRec.
 */
static void release_ReadStreamRec(struct ReadStreamRec *theReadStreamRec)
{
	int mutexerror;
	
	/* grab gNetworkGlobals_lock */
	mutexerror = pthread_mutex_lock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_lock, webdav_kill(-1));
	
	/* release theReadStreamRec */
	theReadStreamRec->inUse = FALSE;
	
	/* release gNetworkGlobals_lock */
	mutexerror = pthread_mutex_unlock(&gNetworkGlobals_lock);
	require_noerr_action(mutexerror, pthread_mutex_unlock, webdav_kill(-1));

pthread_mutex_unlock:
pthread_mutex_lock:

	return;
}

/******************************************************************************/

/*
 * SecCertificateCreateCFData
 *
 * Creates a CFDataRef from a SecCertificateRef.
 */
static CFDataRef SecCertificateCreateCFData(SecCertificateRef cert)
{
	CSSM_DATA cert_data;
	CFDataRef data;
	OSStatus status;

	status = SecCertificateGetData(cert, &cert_data);
	require_noerr_action(status, SecCertificateGetData, data = NULL);
	
	data = CFDataCreate(NULL, cert_data.Data, cert_data.Length);
 
 SecCertificateGetData:
 
	return (data);
}

/******************************************************************************/

/*
 * SecCertificateArrayCreateCFDataArray
 *
 * Convert a CFArray[SecCertificate] to CFArray[CFData].
 */
static CFArrayRef SecCertificateArrayCreateCFDataArray(CFArrayRef certs)
{
	CFMutableArrayRef array;
	int count;
	int i;
	const void *certRef;

	count = CFArrayGetCount(certs);
	array = CFArrayCreateMutable(NULL, count, &kCFTypeArrayCallBacks);
	require(array != NULL, CFArrayCreateMutable);
	
	for (i = 0; i < count; ++i)
	{
		SecCertificateRef cert;
		CFDataRef data;

		certRef = CFArrayGetValueAtIndex(certs, i);
		cert = *((SecCertificateRef*)((void*)&certRef)); /* ugly but it works */
		require(cert != NULL, CFArrayGetValueAtIndex);
		
		data = SecCertificateCreateCFData(cert);
		require(data != NULL, SecCertificateCreateCFData);
		
		CFArrayAppendValue(array, data);
		CFRelease(data);
	}
	
	return (array);
	
	/************/

SecCertificateCreateCFData:
CFArrayGetValueAtIndex:
	CFRelease(array);
CFArrayCreateMutable:

	return (NULL);
}

/******************************************************************************/

/* returns TRUE if user asked to continue with this certificate problem; FALSE if not */
static int ConfirmCertificate(CFReadStreamRef readStreamRef, SInt32 error)
{
	int result;
	CFMutableDictionaryRef dict;
	CFArrayRef certs;
	CFArrayRef certs_data;
	CFNumberRef error_number;
	CFStringRef host_name;
	CFDataRef theData;
	int fd[2];
	int pid, terminated_pid;
	union wait status;
	char *env[] = {"__CF_USER_TEXT_ENCODING=0x1D29:0:0", "", (char *) 0 };

	result = FALSE;
	fd[0] = fd[1] = -1;
	
	/* create a dictionary to stuff things all in */
	dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	require(dict != NULL, CFDictionaryCreateMutable);
	
	/* get the certificates from the stream and add it with the kSSLClientPropTLSServerCertificateChain key */
	certs = (CFArrayRef)CFReadStreamCopyProperty(readStreamRef, kCFStreamPropertySSLPeerCertificates);
	require(certs != NULL, CFReadStreamCopyProperty);
	
	certs_data = SecCertificateArrayCreateCFDataArray(certs);
	CFRelease(certs);
	require(certs_data != NULL, CFReadStreamCopyProperty);

	CFDictionaryAddValue(dict, kSSLClientPropTLSServerCertificateChain, certs_data);
	CFRelease(certs_data);
	
	/* convert error to a CFNumberRef and add it with the kSSLClientPropTLSTrustClientStatus key */
	error_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &error);
	require(error_number != NULL, CFNumberCreate);

	CFDictionaryAddValue(dict, kSSLClientPropTLSTrustClientStatus, error_number);
	CFRelease(error_number);
	
	/* get the host name from the base URL and add it with the kSSLClientPropTLSServerHostName key */
	host_name = CFURLCopyHostName(gBaseURL);
	require(host_name != NULL, CFURLCopyHostName);
	
	CFDictionaryAddValue(dict, kSSLClientPropTLSServerHostName, host_name);
	CFRelease(host_name);
	
	/* flatten it */
	theData = CFPropertyListCreateXMLData(kCFAllocatorDefault, dict);
	require(theData != NULL, CFPropertyListCreateXMLData);

	CFRelease(dict);
	dict = NULL;
	
	/* open a pipe */
	require(pipe(fd) >= 0, pipe);
	
	pid = fork();
	require (pid >= 0, fork);
	if ( pid > 0 )
	{
		/* parent */
		size_t length;
		ssize_t bytes_written;
		
		close(fd[0]); /* close read end */
		fd[0] = -1;
		length = CFDataGetLength(theData);
		bytes_written = write(fd[1], CFDataGetBytePtr(theData), length);
		require(bytes_written == (ssize_t)length, write);
		
		close(fd[1]); /* close write end */
		fd[1] = -1;

		/* Parent waits for child's completion here */
		while ( (terminated_pid = wait4(pid, (int *)&status, 0, NULL)) < 0 )
		{
			/* retry if EINTR, else break out with error */
			if ( errno != EINTR )
			{
				break;
			}
		}
		
		/* we'll get here when the child completes */
		if ( (terminated_pid == pid) && (WIFEXITED(status)) )
		{
			result = WEXITSTATUS(status) == 0;
		}
		else
		{
			result = FALSE;
		}
	}
	else
	{
		/* child */
		close(fd[1]); /* close write end */
		fd[1] = -1;
		
		if ( fd[0] != STDIN_FILENO )
		{
			require(dup2(fd[0], STDIN_FILENO) == STDIN_FILENO, dup2);
			close(fd[0]); /* not needed after dup2 */
			fd[0] = -1;
		}
		
		require(execle(PRIVATE_CERT_UI_COMMAND, PRIVATE_CERT_UI_COMMAND, (char *) 0, env) >= 0, execl);
	}

	return ( result );

	/************/

execl:
dup2:
write:
fork:
	if (fd[0] != -1)
	{
		close(fd[0]);
	}
	if (fd[1] != -1)
	{
		close(fd[1]);
	}
pipe:
CFPropertyListCreateXMLData:
CFURLCopyHostName:
CFNumberCreate:
CFReadStreamCopyProperty:
	if ( dict != NULL )
	{
		CFRelease(dict);
	}
CFDictionaryCreateMutable:

	return ( FALSE );
}

/******************************************************************************/

/*
 * returns EAGAIN if entire transaction should be retried
 * returns ECANCELED if the user clicked cancel in the certificate UI
 * returns EIO if this was not an SSL error
 */
static int HandleSSLErrors(CFReadStreamRef readStreamRef)
{
	CFStreamError streamError;
	SInt32 error;
	int result;
	
	result = EIO;

	streamError = CFReadStreamGetError(readStreamRef);
	if ( streamError.domain == kCFStreamErrorDomainSSL )
	{
		error = streamError.error;
				
		if ( gSSLPropertiesDict == NULL )
		{
			gSSLPropertiesDict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			require(gSSLPropertiesDict != NULL, CFDictionaryCreateMutable);
		}
		
		/* if we haven't tried falling back from TLS to SSL and the errror indicates that might work... */
		if ( (CFDictionaryGetValue(gSSLPropertiesDict, kCFStreamSSLLevel) == NULL) &&
				(((error <= errSSLProtocol) && (error > errSSLXCertChainInvalid)) ||
				 ((error <= errSSLCrypto) && (error > errSSLUnknownRootCert)) ||
				 ((error <= errSSLClosedNoNotify) && (error > errSSLPeerBadCert)) ||
				 (error == errSSLIllegalParam) ||
				 ((error <= errSSLPeerAccessDenied) && (error > errSSLLast))) )
		{
			/* retry with fall back from TLS to SSL */
			CFDictionarySetValue(gSSLPropertiesDict, kCFStreamSSLLevel, kCFStreamSocketSecurityLevelSSLv3);
			result = EAGAIN;
		}
		else
		{
			switch ( error )
			{
				case errSSLCertExpired:
				case errSSLCertNotYetValid:
					/* The certificate for this server has expired or is not yet valid */
					if ( (CFDictionaryGetValue(gSSLPropertiesDict, kCFStreamSSLAllowsExpiredCertificates) == NULL) )
					{
						if ( ConfirmCertificate(readStreamRef, error) )
						{
							result = EAGAIN;
							CFDictionarySetValue(gSSLPropertiesDict, kCFStreamSSLAllowsExpiredCertificates, kCFBooleanTrue);
							CFDictionarySetValue(gSSLPropertiesDict, kCFStreamSSLAllowsExpiredRoots, kCFBooleanTrue);
						}
						else
						{
							result = ECANCELED;
						}
					}
					break;
					
				case errSSLBadCert:
				case errSSLXCertChainInvalid:
				case errSSLHostNameMismatch:
					/* The certificate for this server is invalid */
					if ( (CFDictionaryGetValue(gSSLPropertiesDict, kCFStreamSSLValidatesCertificateChain) == NULL) )
					{
						if ( ConfirmCertificate(readStreamRef, error) )
						{
							result = EAGAIN;
							CFDictionarySetValue(gSSLPropertiesDict, kCFStreamSSLValidatesCertificateChain, kCFBooleanFalse);
						}
						else
						{
							result = ECANCELED;
						}
					}
					break;
					
				case errSSLUnknownRootCert:
				case errSSLNoRootCert:
					/* The certificate for this server was signed by an unknown certifying authority */
					if ( (CFDictionaryGetValue(gSSLPropertiesDict, kCFStreamSSLAllowsAnyRoot) == NULL) )
					{
						if ( ConfirmCertificate(readStreamRef, error) )
						{
							result = EAGAIN;
							CFDictionarySetValue(gSSLPropertiesDict, kCFStreamSSLAllowsAnyRoot, kCFBooleanTrue);
						}
						else
						{
							result = ECANCELED;
						}
					}
					break;
					
				default:
					result = EIO;
					break;
			}
		}
	}

CFDictionaryCreateMutable:
	
	return ( result );
}

/******************************************************************************/

	/* create the HTTP read stream with CFReadStreamCreateForHTTPRequest */
	/* turn on automatic redirection */
	/* add proxies (if any) */
	/* apply any SSL properties we've already negotiated with the server */
	/* open the read stream */
	
	
	/* set the file position to 0 */
	/* create a stream from the file */
	/* create the HTTP read stream with CFReadStreamCreateForStreamedHTTPRequest */
	/* Note: PUTs cannot be automatically redirected -- see rfc2616 section 10.3.x */
	/* add proxies (if any) */
	/* apply any SSL properties we've already negotiated with the server */
	/* open the read stream */


	/* create the HTTP read stream with CFReadStreamCreateForHTTPRequest */
	/* (conditional) turn on automatic redirection */
	/* add proxies (if any) */
	/* apply any SSL properties we've already negotiated with the server */
	/* open the read stream and handle SSL errors */
	
static int open_stream_for_transaction(
	CFHTTPMessageRef request,	/* -> the request to send */
	CFReadStreamRef fdStream,	/* -> if not NULL, the file stream */
	int auto_redirect,			/* -> if TRUE, set kCFStreamPropertyHTTPShouldAutoredirect on stream */
	int *retryTransaction,		/* -> if TRUE, return EAGAIN on errors when streamError is kCFStreamErrorDomainPOSIX/EPIPE and set retryTransaction to FALSE */ 
	struct ReadStreamRec **readStreamRecPtr)	/* <- pointer to the ReadStreamRec in use */
{
	int result;
	struct ReadStreamRec *theReadStreamRec;
	CFReadStreamRef newReadStreamRef;
	
	result = 0;
	*readStreamRecPtr = NULL;
	
	/* create the HTTP read stream */
	if ( fdStream != NULL )
	{
		newReadStreamRef = CFReadStreamCreateForStreamedHTTPRequest(kCFAllocatorDefault, request, fdStream);
		require(newReadStreamRef != NULL, CFReadStreamCreateForStreamedHTTPRequest);
	}
	else
	{
		newReadStreamRef = CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request);
		require(newReadStreamRef != NULL, CFReadStreamCreateForHTTPRequest);
	}
	
	/* add persistent property */
	CFReadStreamSetProperty(newReadStreamRef, kCFStreamPropertyHTTPAttemptPersistentConnection, kCFBooleanTrue);

	/* turn on automatic redirection */
	if ( auto_redirect )
	{
		require(CFReadStreamSetProperty(newReadStreamRef, kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanTrue) != FALSE, SetAutoredirectProperty);
	}

	/* add proxies (if any) */
	require_quiet(set_global_stream_properties(newReadStreamRef) == 0, set_global_stream_properties);

	/* apply any SSL properties we've already negotiated with the server */
	ApplySSLProperties(newReadStreamRef);

	/* get a ReadStreamRec that was not in use */
	theReadStreamRec = get_ReadStreamRec();
	
	/* (after unlocking) make sure we got a ReadStreamRec */
	require(theReadStreamRec != NULL, get_ReadStreamRec);
	
	/* add the unique property from the ReadStreamRec to newReadStreamRef */
	require(CFReadStreamSetProperty(newReadStreamRef, CFSTR("WebdavConnectionNumber"), theReadStreamRec->uniqueValue) != FALSE, SetWebdavConnectionNumberProperty);
	
	/* open the read stream and handle SSL errors */
	if ( CFReadStreamOpen(newReadStreamRef) == FALSE )
	{
		result = HandleSSLErrors(newReadStreamRef);
		if ( result != EAGAIN )
		{
			CFStreamError streamError;
			
			streamError = CFReadStreamGetError(newReadStreamRef);
			if ( *retryTransaction && streamError.domain == kCFStreamErrorDomainPOSIX && streamError.error == EPIPE)
			{
				/* if we get a POSIX errror back from the stream, retry the transaction once */
				syslog(LOG_INFO,"open_stream_for_transaction: CFStreamError: domain %ld, error %ld -- retrying", streamError.domain, streamError.error);
				*retryTransaction = FALSE;
				result = EAGAIN;
			}
			else
			{
				if ( get_connectionstate() == WEBDAV_CONNECTION_UP )
				{
					syslog(LOG_ERR,"open_stream_for_transaction: CFStreamError: domain %ld, error %ld", streamError.domain, streamError.error);
				}
				set_connectionstate(WEBDAV_CONNECTION_DOWN);
				result = ENXIO;
			}
		}
		goto CFReadStreamOpen;
	}
	
	/* close and release old read stream */
	if ( theReadStreamRec->readStreamRef != NULL )
	{
		CFReadStreamClose(theReadStreamRec->readStreamRef);
		CFRelease(theReadStreamRec->readStreamRef);
	}
	
	/* save new read stream */
	theReadStreamRec->readStreamRef = newReadStreamRef;
	
	/* return the ReadStreamRec to the caller */
	*readStreamRecPtr = theReadStreamRec;
	
	return ( 0 );

	/**********************/

CFReadStreamOpen:
SetWebdavConnectionNumberProperty:
	
	release_ReadStreamRec(theReadStreamRec);
	
get_ReadStreamRec:
set_global_stream_properties:
SetAutoredirectProperty:
	
	CFRelease(newReadStreamRef);
	
CFReadStreamCreateForHTTPRequest:	
CFReadStreamCreateForStreamedHTTPRequest:
	
	*readStreamRecPtr = NULL;
	if ( result == 0 )
	{
		result = EIO;
	}
	
	return ( result );
}

/******************************************************************************/

/*
 * stream_get_transaction
 *
 * Creates an HTTP stream, sends the request and returns the response and response body.
 */
static int stream_get_transaction(
	CFHTTPMessageRef request,	/* -> the request to send */
	int *retryTransaction,		/* -> if TRUE, return EAGAIN on errors when streamError is kCFStreamErrorDomainPOSIX/EPIPE and set retryTransaction to FALSE */ 
	struct node_entry *node,	/* -> node to get into */
	CFHTTPMessageRef *response)
{
	struct ReadStreamRec *readStreamRecPtr;
	UInt8 *buffer;
	CFIndex totalRead;
	CFIndex bytesRead;
	CFTypeRef theResponsePropertyRef;
	int background_load;
	CFStringRef connectionHeaderRef;
	CFHTTPMessageRef responseMessage;
	int result;
	
	result = 0;
		
	/*
	 * If we're down and the mount is supposed to fail on disconnects
	 * instead of retrying, just return an error.
	 */
	require_quiet(!gSuppressAllUI || (get_connectionstate() == WEBDAV_CONNECTION_UP), connection_down);
	
	result = open_stream_for_transaction(request, NULL, TRUE, retryTransaction, &readStreamRecPtr);
	require_noerr_quiet(result, open_stream_for_transaction);
	
	/* malloc a buffer big enough for first read */
	buffer = malloc(first_read_len);
	require(buffer != NULL, malloc_buffer);

	/* Send the message and get up to first_read_len bytes of response */
	totalRead = 0;
	background_load = FALSE;
	while ( 1 )
	{
		bytesRead = CFReadStreamRead(readStreamRecPtr->readStreamRef, buffer + totalRead, first_read_len - totalRead);
		if ( bytesRead > 0 )
		{
			totalRead += bytesRead;
			if ( totalRead >= first_read_len )
			{
				/* is there more data to read? */
				if ( CFReadStreamGetStatus(readStreamRecPtr->readStreamRef) == kCFStreamStatusAtEnd )
				{
					/* there are no more bytes to read */
					background_load = FALSE;
				}
				else
				{
					/* we'll need to hand off to get the rest of the file data */
					background_load = TRUE;
				}
				break;
			}
		}
		else if ( bytesRead == 0 )
		{
			/* there are no more bytes to read */
			background_load = FALSE;
			break;
		}
		else
		{
			CFStreamError streamError;
			
			streamError = CFReadStreamGetError(readStreamRecPtr->readStreamRef);
			if ( *retryTransaction && streamError.domain == kCFStreamErrorDomainPOSIX && streamError.error == EPIPE )
			{
				/* if we get a POSIX errror back from the stream, retry the transaction once */
				syslog(LOG_INFO,"stream_get_transaction: CFStreamError: domain %ld, error %ld -- retrying", streamError.domain, streamError.error);
				*retryTransaction = FALSE;
				result = EAGAIN;
			}
			else
			{
				if ( get_connectionstate() == WEBDAV_CONNECTION_UP )
				{
					syslog(LOG_ERR,"stream_get_transaction: CFStreamError: domain %ld, error %ld", streamError.domain, streamError.error);
				}
				set_connectionstate(WEBDAV_CONNECTION_DOWN);
				result = ENXIO;
			}
			goto CFReadStreamRead;
		}
	};
	
	/* get the response header */
	theResponsePropertyRef = CFReadStreamCopyProperty(readStreamRecPtr->readStreamRef, kCFStreamPropertyHTTPResponseHeader);
	require(theResponsePropertyRef != NULL, GetResponseHeader);
	
	/* fun with casting a "const void *" CFTypeRef away */
	responseMessage = *((CFHTTPMessageRef*)((void*)&theResponsePropertyRef));

	/*
	 *	if WEBDAV_DOWNLOAD_NEVER
	 *		download entire file
	 *	else if WEBDAV_DOWNLOAD_FINISHED
	 *		then use If-Modified-Since: node->file_last_modified date
	 *		//and  If-None-Match node->file_entity_tag to the cache file
	 *			200 = getting whole file
	 *			304 = not modified; current copy is OK
	 *	//else if has node->file_entity_tag and NOT weak
	 *	//	download partial file with If-Range: node->file_entity_tag
	 *	//		206 = getting remainder
	 *	//		200 = getting whole file
	 *	else //if has node->file_last_modified
	 *		download partial file with If-Range: node->file_last_modified date
	 *			206 = getting remainder
	 *			200 = getting whole file
	 */
	/* get the status code */
	switch ( CFHTTPMessageGetResponseStatusCode(responseMessage) )
	{
		case 200:	/* OK - download whole file from beginning */
			/* clear the flags */
			require(fchflags(node->file_fd, 0) == 0, fchflags);
			/* truncate the file */
			require_action(ftruncate(node->file_fd, 0LL) != -1, ftruncate, syslog(LOG_ERR,"errno %d", errno));
			/* reset the position to 0 */
			require(lseek(node->file_fd, 0LL, SEEK_SET) >= 0, lseek);
			/* write the bytes in buffer to the cache file*/
			require(write(node->file_fd, buffer, (size_t)totalRead) == (ssize_t)totalRead, write);
			break;
			
		case 206:	/* Partial Content - download from EOF */
			/* clear the flags */
			require(fchflags(node->file_fd, 0) == 0, fchflags);
			/* seek to EOF */
			require(lseek(node->file_fd, 0LL, SEEK_END) >= 0, lseek);
			/* write the bytes in buffer to the cache file*/
			require(write(node->file_fd, buffer, (size_t)totalRead) >= 0, write);
			break;
			
		case 304:	/* Not Modified - the cache file is still good */
			background_load = FALSE;
			break;
			
		default:
			background_load = FALSE;
			break;
	}
	
	free(buffer);
	buffer = NULL;
	
	set_connectionstate(WEBDAV_CONNECTION_UP);
	
	/* Get the Connection header (if any) */
	readStreamRecPtr->connectionClose = FALSE;
	connectionHeaderRef = CFHTTPMessageCopyHeaderFieldValue(responseMessage, CFSTR("Connection"));
	if ( connectionHeaderRef != NULL )
	{
		/* is the connection-token is "close"? */
		if ( CFStringCompare(connectionHeaderRef, CFSTR("close"), kCFCompareCaseInsensitive) == kCFCompareEqualTo )
		{
			/* yes -- then the server closed this connection, so close and release the read stream now */
			readStreamRecPtr->connectionClose = TRUE;
		}
		CFRelease(connectionHeaderRef);
	}
	
	if ( background_load )
	{
		int error;
		/*
		 * As a hack, set the NODUMP bit so that the kernel
		 * knows that we are in the process of filling up the file
		 */
		require(fchflags(node->file_fd, UF_NODUMP) == 0, fchflags);
		
		node->file_status = WEBDAV_DOWNLOAD_IN_PROGRESS;
		
		/* pass the node and readStreamRef off to another thread to finish */
		error = requestqueue_enqueue_download(node, readStreamRecPtr);
		require_noerr_quiet(error, webdav_requestqueue_enqueue_new_download);
	}
	else
	{
		node->file_status = WEBDAV_DOWNLOAD_FINISHED;
		
		if ( readStreamRecPtr->connectionClose )
		{
			/* close and release the stream */
			CFReadStreamClose(readStreamRecPtr->readStreamRef);
			CFRelease(readStreamRecPtr->readStreamRef);
			readStreamRecPtr->readStreamRef = NULL;
		}
		
		/* make this ReadStreamRec is available again */
		release_ReadStreamRec(readStreamRecPtr);
	}
	
	*response = responseMessage;
	
	return ( 0 );

	/**********************/

webdav_requestqueue_enqueue_new_download:
fchflags:
ftruncate:
lseek:
write:
GetResponseHeader:
CFReadStreamRead:

	if ( buffer != NULL )
	{
		free(buffer);
	}

malloc_buffer:

	/* close and release the read stream on errors */
	CFReadStreamClose(readStreamRecPtr->readStreamRef);
	CFRelease(readStreamRecPtr->readStreamRef);
	readStreamRecPtr->readStreamRef = NULL;
	
	/* make this ReadStreamRec is available again */
	release_ReadStreamRec(readStreamRecPtr);

open_stream_for_transaction:
connection_down:

	*response = NULL;

	if ( result == 0 )
	{
		result = EIO;
	}

	return ( result );
}

/******************************************************************************/

/*
 * stream_transaction_from_file
 *
 * Creates an HTTP stream with the read stream coming from file_fd,
 * sends the request and returns the response. The response body (if any) is
 * read and disposed.
 */
static int stream_transaction_from_file(
	CFHTTPMessageRef request,
	int file_fd,
	int *retryTransaction,		/* -> if TRUE, return EAGAIN on errors when streamError is kCFStreamErrorDomainPOSIX/EPIPE and set retryTransaction to FALSE */ 
	CFHTTPMessageRef *response)
{
	CFReadStreamRef fdStream;
	struct ReadStreamRec *readStreamRecPtr;
	void *buffer;
	CFIndex bytesRead;
	CFTypeRef theResponsePropertyRef;
	off_t contentLength;
	CFStringRef contentLengthString;
	CFStringRef connectionHeaderRef;
	CFHTTPMessageRef responseMessage;
	int result;
	
	result = 0;
		
	/*
	 * If we're down and the mount is supposed to fail on disconnects
	 * instead of retrying, just return an error.
	 */
	require_quiet(!gSuppressAllUI || (get_connectionstate() == WEBDAV_CONNECTION_UP), connection_down);
	
	/* get the file length */
	contentLength = lseek(file_fd, 0LL, SEEK_END);
	require(contentLength != -1, lseek);
	
	/* create a string with the file length for the Content-Length header */
	contentLengthString = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%qd"), contentLength);
	/* CFReadStreamCreateForStreamedHTTPRequest will use chunked transfer-encoding if the Content-Length header cannot be provided */
	if ( contentLengthString != NULL )
	{
		CFHTTPMessageSetHeaderFieldValue(request, CFSTR("Content-Length"), contentLengthString);
		CFRelease(contentLengthString);
	}
	
	/* set the file position to 0 */
	verify(lseek(file_fd, 0LL, SEEK_SET) != -1);
	
	/* create a stream from the file */
	CFStreamCreatePairWithSocket(kCFAllocatorDefault, file_fd, &fdStream, NULL);
	require(fdStream != NULL, CFReadStreamCreateWithFile);
	
	result = open_stream_for_transaction(request, fdStream, FALSE, retryTransaction, &readStreamRecPtr);
	require_noerr_quiet(result, open_stream_for_transaction);
		
	/* malloc a buffer big enough for most responses */
	buffer = malloc(BODY_BUFFER_SIZE);
	require(buffer != NULL, malloc_currentbuffer);

	/* Send the message and eat the response */
	while ( 1 )
	{
		bytesRead = CFReadStreamRead(readStreamRecPtr->readStreamRef, buffer, BODY_BUFFER_SIZE);
		if ( bytesRead > 0 )
		{
			continue;
		}
		else if ( bytesRead == 0 )
		{
			/* there are no more bytes to read */
			break;
		}
		else
		{
			CFStreamError streamError;
						
			streamError = CFReadStreamGetError(readStreamRecPtr->readStreamRef);
			if ( *retryTransaction && streamError.domain == kCFStreamErrorDomainPOSIX && streamError.error == EPIPE )
			{
				/* if we get a POSIX errror back from the stream, retry the transaction once */
				syslog(LOG_INFO,"stream_transaction_from_file: CFStreamError: domain %ld, error %ld -- retrying", streamError.domain, streamError.error);
				*retryTransaction = FALSE;
				result = EAGAIN;
			}
			else
			{
				if ( get_connectionstate() == WEBDAV_CONNECTION_UP )
				{
					syslog(LOG_ERR,"stream_transaction_from_file: CFStreamError: domain %ld, error %ld", streamError.domain, streamError.error);
				}
				set_connectionstate(WEBDAV_CONNECTION_DOWN);
				result = ENXIO;
			}
			goto CFReadStreamRead;
		}
	};
	
	free(buffer);

	/* get the response header */
	theResponsePropertyRef = CFReadStreamCopyProperty(readStreamRecPtr->readStreamRef, kCFStreamPropertyHTTPResponseHeader);
	require(theResponsePropertyRef != NULL, GetResponseHeader);
	
	/* fun with casting a "const void *" CFTypeRef away */
	responseMessage = *((CFHTTPMessageRef*)((void*)&theResponsePropertyRef));
	
	set_connectionstate(WEBDAV_CONNECTION_UP);
	
	/* Get the Connection header (if any) */
	connectionHeaderRef = CFHTTPMessageCopyHeaderFieldValue(responseMessage, CFSTR("Connection"));
	/* is the connection-token is "close"? */
	if ( connectionHeaderRef != NULL )
	{
		if ( CFStringCompare(connectionHeaderRef, CFSTR("close"), kCFCompareCaseInsensitive) == kCFCompareEqualTo )
		{
			/* yes -- then the server closed this connection, so close and release the read stream now */
			CFReadStreamClose(readStreamRecPtr->readStreamRef);
			CFRelease(readStreamRecPtr->readStreamRef);
			readStreamRecPtr->readStreamRef = NULL;
		}
		CFRelease(connectionHeaderRef);
	}

	CFRelease(fdStream);

	/* make this ReadStreamRec is available again */
	release_ReadStreamRec(readStreamRecPtr);
	
	/* fun with casting a "const void *" CFTypeRef away */
	*response = responseMessage;
	
	return ( 0 );

	/**********************/

GetResponseHeader:
CFReadStreamRead:

	free(buffer);

malloc_currentbuffer:

	CFReadStreamClose(readStreamRecPtr->readStreamRef);
	CFRelease(readStreamRecPtr->readStreamRef);
	readStreamRecPtr->readStreamRef = NULL;
	
	/* make this ReadStreamRec is available again */
	release_ReadStreamRec(readStreamRecPtr);

	CFRelease(fdStream);

CFReadStreamCreateWithFile:

lseek:
open_stream_for_transaction:
connection_down:

	*response = NULL;

	if ( result == 0 )
	{
		result = EIO;
	}

	return ( result );
}

/******************************************************************************/

/*
 * stream_transaction
 *
 * Creates an HTTP stream, sends the request and returns the response and response body.
 */
static int stream_transaction(
	CFHTTPMessageRef request,	/* -> the request to send */
	int auto_redirect,			/* -> if TRUE, set kCFStreamPropertyHTTPShouldAutoredirect on stream */
	int *retryTransaction,		/* -> if TRUE, return EAGAIN on errors when streamError is kCFStreamErrorDomainPOSIX/EPIPE and set retryTransaction to FALSE */ 
	UInt8 **buffer,				/* <- response data buffer (caller responsible for freeing) */
	CFIndex *count,				/* <- response data buffer length */
	CFHTTPMessageRef *response)	/* <- the response message */
{
	struct ReadStreamRec *readStreamRecPtr;
	CFIndex	totalRead;
	UInt8 *currentbuffer;
	UInt8 *newBuffer;
	CFIndex bytesRead;
	CFIndex bytesToRead;
	CFIndex bufferSize;
	CFTypeRef theResponsePropertyRef;
	CFStringRef connectionHeaderRef;
	CFHTTPMessageRef responseMessage;
	int result;
	
	result = 0;
	
	/*
	 * If we're down and the mount is supposed to fail on disconnects
	 * instead of retrying, just return an error.
	 */
	require_quiet(!gSuppressAllUI || (get_connectionstate() == WEBDAV_CONNECTION_UP), connection_down);
	
	/* get an open ReadStreamRec */
	result = open_stream_for_transaction(request, NULL, auto_redirect, retryTransaction, &readStreamRecPtr);
	require_noerr_quiet(result, open_stream_for_transaction);
	
	/* malloc a buffer big enough for most responses */
	bufferSize = BODY_BUFFER_SIZE;
	currentbuffer = malloc(bufferSize);
	require(currentbuffer != NULL, malloc_currentbuffer);

	/* Send the message and get the response */
	totalRead = 0;
	while ( 1 )
	{
		bytesToRead = bufferSize - totalRead;
		bytesRead = CFReadStreamRead(readStreamRecPtr->readStreamRef, currentbuffer + totalRead, bytesToRead);
		if ( bytesRead > 0 )
		{
			totalRead += bytesRead;
			
			/* is currentbuffer getting close to full? */
			if ( (bytesToRead - bytesRead) < (BODY_BUFFER_SIZE / 2) )
			{
				/* yes, so get larger currentbuffer for next read */
				bufferSize += BODY_BUFFER_SIZE;
				newBuffer = realloc(currentbuffer, bufferSize);
				require(newBuffer != NULL, realloc);
				
				currentbuffer = newBuffer;
			}
		}
		else if ( bytesRead == 0 )
		{
			/* there are no more bytes to read */
			break;
		}
		else
		{
			result = HandleSSLErrors(readStreamRecPtr->readStreamRef);
			if ( result != EAGAIN )
			{
				if ( result != ECANCELED )
				{
					CFStreamError streamError;
								
					streamError = CFReadStreamGetError(readStreamRecPtr->readStreamRef);
					if ( *retryTransaction && streamError.domain == kCFStreamErrorDomainPOSIX && streamError.error == EPIPE )
					{
						/* if we get a POSIX errror back from the stream, retry the transaction once */
						syslog(LOG_INFO,"stream_transaction: CFStreamError: domain %ld, error %ld -- retrying", streamError.domain, streamError.error);
						*retryTransaction = FALSE;
						result = EAGAIN;
					}
					else
					{
						if ( get_connectionstate() == WEBDAV_CONNECTION_UP )
						{
							syslog(LOG_ERR,"stream_transaction: CFStreamError: domain %ld, error %ld", streamError.domain, streamError.error);
						}
						set_connectionstate(WEBDAV_CONNECTION_DOWN);
						result = ENXIO;
					}
				}
			}
			goto CFReadStreamRead;
		}
	};
	
	/* get the response header */
	theResponsePropertyRef = CFReadStreamCopyProperty(readStreamRecPtr->readStreamRef, kCFStreamPropertyHTTPResponseHeader);
	require(theResponsePropertyRef != NULL, GetResponseHeader);
	
	/* fun with casting a "const void *" CFTypeRef away */
	responseMessage = *((CFHTTPMessageRef*)((void*)&theResponsePropertyRef));
	
	set_connectionstate(WEBDAV_CONNECTION_UP);
	
	/* Get the Connection header (if any) */
	connectionHeaderRef = CFHTTPMessageCopyHeaderFieldValue(responseMessage, CFSTR("Connection"));
	/* is the connection-token is "close"? */
	if ( connectionHeaderRef != NULL )
	{
		if ( CFStringCompare(connectionHeaderRef, CFSTR("close"), kCFCompareCaseInsensitive) == kCFCompareEqualTo )
		{
			/* yes -- then the server closed this connection, so close and release the read stream now */
			CFReadStreamClose(readStreamRecPtr->readStreamRef);
			CFRelease(readStreamRecPtr->readStreamRef);
			readStreamRecPtr->readStreamRef = NULL;
		}
		CFRelease(connectionHeaderRef);
	}
	
	/* make this ReadStreamRec is available again */
	release_ReadStreamRec(readStreamRecPtr);
		
	*response = responseMessage;
	*count = totalRead;
	*buffer = currentbuffer;
	
	return ( 0 );

	/**********************/

GetResponseHeader:
CFReadStreamRead:
realloc:

	free(currentbuffer);

malloc_currentbuffer:

	/* close and release the read stream on errors */
	CFReadStreamClose(readStreamRecPtr->readStreamRef);
	CFRelease(readStreamRecPtr->readStreamRef);
	readStreamRecPtr->readStreamRef = NULL;
	
	/* make this ReadStreamRec is available again */
	release_ReadStreamRec(readStreamRecPtr);

open_stream_for_transaction:
connection_down:

	*response = NULL;
	*count = 0;
	*buffer = NULL;
	if ( result == 0 )
	{
		result = EIO;
	}
	
	return ( result );
}

/******************************************************************************/

/*
 * send_transaction
 *
 * Creates a request, adds the message body, headers and authentication if needed,
 * and then calls stream_transaction() to send the request to the server and get
 * the server's response. If the caller requests the response body and/or the
 * response message, they are returned. Otherwise, they are freed/released.
 */
static int send_transaction(
	uid_t uid,							/* -> uid of the user making the request */
	CFURLRef url,						/* -> url to the resource */
	CFStringRef requestMethod,			/* -> the request method */
	CFDataRef bodyData,					/* -> message body data, or NULL if no body */
	CFIndex headerCount,				/* -> number of headers */
	struct HeaderFieldValue *headers,	/* -> pointer to array of struct HeaderFieldValue, or NULL if none */
	int auto_redirect,					/* -> if TRUE, set kCFStreamPropertyHTTPShouldAutoredirect on stream */
	UInt8 **buffer,						/* <- if not NULL, response data buffer is returned here (caller responsible for freeing) */
	CFIndex *count,						/* <- if not NULL, response data buffer length is returned here*/
	CFHTTPMessageRef *response)			/* <- if not NULL, response is returned here */
{
	int error;
	CFIndex i;
	struct HeaderFieldValue *headerPtr;
	CFHTTPMessageRef message;
	CFHTTPMessageRef responseRef;
	UInt32 statusCode;
	UInt32 auth_generation;
	UInt8 *responseBuffer;
	CFIndex responseBufferLength;
	int retryTransaction;
	
	error = 0;
	responseBuffer = NULL;
	responseBufferLength = 0;
	message = NULL;
	responseRef = NULL;
	statusCode = 0;
	auth_generation = 0;
	retryTransaction = TRUE;

	/* the transaction/authentication loop */
	do
	{
		/* release message if left from previous loop */
		if ( message != NULL )
		{
			CFRelease(message);
			message = NULL;
		}
		/* create a CFHTTP message object */
		message = CFHTTPMessageCreateRequest(kCFAllocatorDefault, requestMethod, url, kCFHTTPVersion1_1);
		require_action(message != NULL, CFHTTPMessageCreateRequest, error = EIO);
		
		/* set the message body (if any) */
		if ( bodyData != NULL )
		{
			CFHTTPMessageSetBody(message, bodyData);
		}
		
		/* change the User-Agent header */
		CFHTTPMessageSetHeaderFieldValue(message, CFSTR("User-Agent"), userAgentHeaderValue);
		
		/* add the X-Source-Id header if needed */
		if ( X_Source_Id_HeaderValue != NULL )
		{
			CFHTTPMessageSetHeaderFieldValue(message, CFSTR("X-Source-Id"), X_Source_Id_HeaderValue);
		}
		
		/* add other HTTP headers (if any) */
		for ( i = 0, headerPtr = headers, headers; i < headerCount; ++i, ++headerPtr )
		{
			CFHTTPMessageSetHeaderFieldValue(message, headerPtr->headerField, headerPtr->value);
		}
		
		/* apply credentials (if any) */
		/*
		 * statusCode will be 401 or 407 and responseRef will not be NULL if we've already been through the loop;
		 * statusCode will be 0 and responseRef will be NULL if this is the first time through.
		 */
		error = authcache_apply(uid, message, statusCode, responseRef, &auth_generation);
		if ( error != 0 )
		{
			break;
		}
		
		/* stream_transaction returns responseRef and responseBuffer so release them if left from previous loop */
		if ( responseBuffer != NULL )
		{
			free(responseBuffer);
			responseBuffer = NULL;
			responseBufferLength = 0;
		}
		if ( responseRef != NULL )
		{
			CFRelease(responseRef);
			responseRef = NULL;
		}
		/* now that everything's ready to send, send it */
		error = stream_transaction(message, auto_redirect, &retryTransaction, &responseBuffer, &responseBufferLength, &responseRef);
		if ( error == EAGAIN )
		{
			statusCode = 0;
			/* responseRef will be left NULL on retries */
		}
		else
		{
			if ( error != 0 )
			{
				break;
			}
			
			/* get the status code */
			statusCode = CFHTTPMessageGetResponseStatusCode(responseRef);
		}
	} while ( error == EAGAIN || statusCode == 401 || statusCode == 407 );

CFHTTPMessageCreateRequest:
	
	if ( error == 0 )
	{
		error = translate_status_to_error(statusCode);
		if ( error == 0 )
		{
			/*
			 * when we get here with no errors, then we need to tell the authcache the
			 * transaction worked so it can mark the credentials valid and, if needed
			 * add the credentials to the keychain. If the auth_generation changed, then
			 * another transaction updated the authcache element after we got it.
			 */
			(void) authcache_valid(uid, message, auth_generation);
		}
		else
		{
			if ( responseBuffer != NULL )
			{
				free(responseBuffer);
				responseBuffer = NULL;
			}
		}
	}
	
	if ( message != NULL )
	{
		CFRelease(message);
	}
	
	/* return requested output parameters */
	if ( buffer != NULL )
	{
		*buffer = responseBuffer;
	}
	else
	{
		if ( responseBuffer != NULL )
		{
			free(responseBuffer);
		}
	}
	
	if ( count != NULL )
	{
		*count = responseBufferLength;
	}
	
	if ( response != NULL )
	{
		/* the caller wants the response and will release it */
		*response = responseRef;
	}
	else
	{
		if ( responseRef != NULL )
		{
			CFRelease(responseRef);
		}
	}
	
	return ( error );
}

/*****************************************************************************/

/*
 * ParseDAVLevel parses a DAV header's field-value (if any) to get the DAV level.
 *	Input:
 *		responsePropertyRef	the response message
 *	Outputs:
 *		dav_level			the dav_level (0 = DAV not supported)
 *
 * The rules for message headers are (rfc 2518, section 9.1):
 *
 *	DAV = "DAV" ":" "1" ["," "2"] ["," 1#extend]
 *	extend = Coded-URL | token
 *
 * (Note: The rules for extend are still not in the rfc - they were taken from
 * messages in the WebDAV discussion list and are needed to interoperability
 * with Apache 2.0 servers which put Coded-URLs in DAV headers.)
 */
static void ParseDAVLevel(CFHTTPMessageRef responsePropertyRef, int *dav_level)
{
	CFStringRef davHeaderRef;
	const char *field_value;
	char buffer[4096];
	const char *token;
	
	*dav_level = 0;
	
	davHeaderRef = CFHTTPMessageCopyHeaderFieldValue(responsePropertyRef, CFSTR("DAV"));
	if ( davHeaderRef )
	{
		field_value = CFStringGetCStringPtr(davHeaderRef, kCFStringEncodingUTF8);
		if ( field_value == NULL )
		{
			if ( CFStringGetCString(davHeaderRef, buffer, 4096, kCFStringEncodingUTF8) )
			{
				field_value = buffer;
			}
		}
		CFRelease(davHeaderRef);
		
		if ( field_value != NULL )
		{
			while ( *field_value != '\0' )
			{
				/* find first non-LWS character */
				field_value = SkipLWS(field_value);
				if ( *field_value == '\0' )
				{
					/* if we're at end of string, break out of main while loop */
					break;
				}
				
				/* is value a token or a Coded-URL? */
				if ( *field_value == '<' )
				{
					/* it's a Coded-URL, so eat it */
					
					/* skip over '<' */
					++field_value;
					
					/* find '>"' marking the end of the quoted-string */
					field_value = SkipCodedURL(field_value);
					
					if ( *field_value != '\0' )
					{
						/* skip over '>' */
						++field_value;
					}
				}
				else
				{
					/* it's a token */
					
					/* mark start of the value token */
					token = field_value;
					
					/* find the end of the value token */
					field_value = SkipToken(field_value);
					
					/* could this token be '1' or '2'? */
					if ( (field_value - token) == 1 )
					{
						if ( (*token == '1') && (*dav_level < 1) )
						{
							*dav_level = 1;
						}
						else if ( *token == '2' && (*dav_level < 2) )
						{
							*dav_level = 2;
						}
					}
				}
				
				/* skip over LWS (if any) */
				field_value = SkipLWS(field_value);
				
				/* if there's any string left after the LWS... */
				if ( *field_value != '\0' )
				{
					/* we should have found a comma */
					if ( *field_value != ',' )
					{
						/* if not, break out of main while loop */
						break;
					}
					
					/* skip over one or more commas */
					while ( *field_value == ',' )
					{
						++field_value;
					}
				}
				
				/*
				 * field_value is now pointing at first character after comma
				 * delimiter, or at end of string
				 */
			}
		}
	}
}

/******************************************************************************/

static int network_getDAVLevel(
	uid_t uid,					/* -> uid of the user making the request */
	CFURLRef urlRef,			/* -> url */
	int *dav_level)				/* <- DAV level */
{
	int error;
	CFHTTPMessageRef response;
	/* the 3 headers */
	CFIndex headerCount = 1;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
	};
		
	*dav_level = 0;
		
	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("OPTIONS"), NULL,
		headerCount, headers, TRUE, NULL, NULL, &response);
	if ( !error )
	{
		/* get the DAV level */
		ParseDAVLevel(response, dav_level);
		
		/* release the response buffer */
		CFRelease(response);
	}

	return ( error );
}

/*****************************************************************************/

static
int get_from_attributes_cache(struct node_entry *node, uid_t uid)
{
	ssize_t size;
	int result;
	
	result = FALSE;	/* default to FALSE */
	
	if ( node_appledoubleheader_valid(node, uid) )
	{
		require(fchflags(node->file_fd, 0) == 0, fchflags);
		require(lseek(node->file_fd, (off_t)0, SEEK_SET) != -1, lseek);
		require(ftruncate(node->file_fd, 0LL) != -1, ftruncate);
		/* we found the AppleDouble header in memcache */
		size = write(node->file_fd, (void *)node->attr_appledoubleheader, APPLEDOUBLEHEADER_LENGTH);
		if ( size != APPLEDOUBLEHEADER_LENGTH )
		{
			debug_string("write failed");
			/* attempt to seek back to start of file, make sure it's empty, and then reset its status */
			(void) lseek(node->file_fd, (off_t)0, SEEK_SET);
			(void) ftruncate(node->file_fd, 0LL);
			node->file_status = WEBDAV_DOWNLOAD_NEVER;
			node->file_validated_time = 0;
			node->file_last_modified = -1;
			if ( node->file_entity_tag != NULL )
			{
				free(node->file_entity_tag);
				node->file_entity_tag = NULL;
			}
		}
		else
		{
			node->file_status = WEBDAV_DOWNLOAD_FINISHED;
			node->file_validated_time = node->attr_appledoubleheader_time;
			node->file_last_modified = (node->attr_stat.st_mtimespec.tv_sec != 0) ? node->attr_stat.st_mtimespec.tv_sec : -1;
			/* ••••• should I get the etag when I get attr_appledoubleheader? probably */ 
			if ( node->file_entity_tag != NULL )
			{
				free(node->file_entity_tag);
				node->file_entity_tag = NULL;
			}
			result = TRUE;
		}
	}

ftruncate:
lseek:
fchflags:
	
	return ( result );
}	

/******************************************************************************/

/*
 * network_stat handles requests from network_lookup, network_getattr
 * and network_mount.
 */
static int network_stat(
	uid_t uid,					/* -> uid of the user making the request */
	CFURLRef urlRef,			/* -> url to the resource */
	struct stat *statbuf)		/* <- stat information is returned in this buffer */
{
	int error;
	UInt8 *responseBuffer;
	CFIndex count;
	CFDataRef bodyData;
	/* the xml for the message body */
	const UInt8 xmlString[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<D:propfind xmlns:D=\"DAV:\">\n"
			"<D:prop>\n"
				"<D:getlastmodified/>\n"
				"<D:getcontentlength/>\n"
				"<D:resourcetype/>\n"
			"</D:prop>\n"
		"</D:propfind>\n";
	/* the 3 headers */
	CFIndex headerCount = 3;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("Content-Type"), CFSTR("text/xml") },
		{ CFSTR("Depth"), CFSTR("0") }
	};
	
	/* create the message body with the xml */
	bodyData = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, xmlString, strlen((const char *)xmlString), kCFAllocatorNull);
	require_action(bodyData != NULL, CFDataCreateWithBytesNoCopy, error = EIO);
	
	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("PROPFIND"), bodyData,
		headerCount, headers, TRUE, &responseBuffer, &count, NULL);
	if ( !error )
	{
		/* parse the statbuf from the response buffer */
		error = parse_stat(responseBuffer, count, statbuf);
		
		/* free the response buffer */
		free(responseBuffer);
	}
	
	/* release the message body */
	CFRelease(bodyData);

CFDataCreateWithBytesNoCopy:
	
	return ( error );
}

/******************************************************************************/

static int network_dir_is_empty(
	uid_t uid,					/* -> uid of the user making the request */
	CFURLRef urlRef)			/* -> url to check */
{
	int error;
	UInt8 *responseBuffer;
	CFIndex count;
	CFDataRef bodyData;
	/* the xml for the message body */
	const UInt8 xmlString[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<D:propfind xmlns:D=\"DAV:\">\n"
			"<D:prop>\n"
				"<D:resourcetype/>\n"
			"</D:prop>\n"
		"</D:propfind>\n";
	/* the 3 headers */
	CFIndex headerCount = 3;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("Content-Type"), CFSTR("text/xml") },
		{ CFSTR("Depth"), CFSTR("1") }
	};
	
	error = 0;
	responseBuffer = NULL;

	/* create the message body with the xml */
	bodyData = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, xmlString, strlen((const char *)xmlString), kCFAllocatorNull);
	require_action(bodyData != NULL, CFDataCreateWithBytesNoCopy, error = EIO);
	
	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("PROPFIND"), bodyData,
		headerCount, headers, TRUE, &responseBuffer, &count, NULL);
	if ( !error )
	{
		int num_entries;
		
		/* parse responseBuffer to get the number of entries */
		error = parse_file_count(responseBuffer, count, &num_entries);
		if ( !error )
		{
			if (num_entries > 1)
			{
				/*
				 * An empty directory will have just one entry for itself as far
				 * as the server is concerned.	If there is more than that we need
				 * to return ENOTEMPTY since we don't allow deleting directories
				 * which have anything in them.
				 */
				error = ENOTEMPTY;
			}
		}
		/* free the response buffer */
		free(responseBuffer);
	}
	
	/* release the message body */
	CFRelease(bodyData);

CFDataCreateWithBytesNoCopy:
	
	return ( error );
}

/******************************************************************************/

int network_lookup(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> parent node */
	char *name,					/* -> filename to find */
	size_t name_length,			/* -> length of name */
	struct stat *statbuf)		/* <- stat information is returned in this buffer except for st_ino */
{
	int error;
	CFURLRef urlRef;
	
	error = 0;
	
	/* create a CFURL to the node plus name */
	urlRef = create_cfurl_from_node(node, name, name_length);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
	
	/* let network_stat do the rest of the work */
	error = network_stat(uid, urlRef, statbuf);
	
	CFRelease(urlRef);
	
create_cfurl_from_node:

	return ( error );
}

/******************************************************************************/

int network_getattr(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> node */
	struct stat *statbuf)		/* <- stat information is returned in this buffer */
{
	int error;
	CFURLRef urlRef;
	
	error = 0;
	
	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
	
	/* let network_stat do the rest of the work */
	error = network_stat(uid, urlRef, statbuf);
	if ( error == 0 )
	{
		/* network_stat gets all of the struct stat fields except for st_ino so fill it in here with the fileid of the node */
		statbuf->st_ino = node->fileid;
	}
	
	CFRelease(urlRef);
	
create_cfurl_from_node:

	return ( error );
}

/******************************************************************************/

/* NOTE: this will do both the OPTIONS and the PROPFIND. */
/* NOTE: if webdavfs is changed to support advlocks, then 
 * server_mount_flags parameter is not needed.
 */
/*
 * The only errors expected from network_mount are:
 *		ECANCELED - the user could not authenticate and cancelled the mount
 *		ENODEV - we could not connect to the server (bad URL, server down, etc.)
 */
int network_mount(
	uid_t uid,					/* -> uid of the user making the request */
	int *server_mount_flags)	/* <- flags to OR in with mount flags (i.e., MNT_RDONLY) */
{
	int error;
	CFURLRef urlRef;
	int dav_level;
	struct stat statbuf;
	
	urlRef = gBaseURL;
	
	error = network_getDAVLevel(uid, urlRef, &dav_level);
	if ( error == 0 )
	{
		if ( dav_level > 2 )
		{
			/* pin it to 2 -- the highest we care about */
			dav_level = 2;
		} 
		switch (dav_level)
		{
			case 1:
				*server_mount_flags |= MNT_RDONLY;
				break;
				
			case 2:
				/* DAV supports LOCKs */
				break;
				
			default:
				debug_string("network_mount: WebDAV protocol not supported");
				error = ENODEV;
				break;
		}
		
		if ( error == 0 )
		{
			error = network_stat(uid, urlRef, &statbuf);
			if ( error )
			{
				if (error != EACCES)
				{
					debug_string("network_mount: PROPFIND failed");
					error = ENODEV;
				}
				else
				{
					debug_string("network_mount: mount cancelled by user");
					error = ECANCELED;
				}
			}
			else if ( !S_ISDIR(statbuf.st_mode) )
			{
				/* the PROFIND was successful, but the URL was to a file, not a collection */
				debug_string("network_mount: URL is not a collection resource (directory)");
				error = ENODEV;
			}
		}
	}
	else
	{
		if ( error != EACCES )
		{
			debug_string("network_mount: OPTIONS failed");
			error = ENODEV;
		}
		else
		{
			debug_string("network_mount: mount cancelled by user");
			error = ECANCELED;
		}
	}
	
	return ( error );
}

/******************************************************************************/

int network_finish_download(
	struct node_entry *node,
	struct ReadStreamRec *readStreamRecPtr)
{
	UInt8 *buffer;
	CFIndex bytesRead;
	
	/* malloc a buffer */
	buffer = malloc(BODY_BUFFER_SIZE);
	require(buffer != NULL, malloc_buffer);

	while ( 1 )
	{
		/* were we asked to terminate the download? */
		if ( (node->file_status & WEBDAV_DOWNLOAD_TERMINATED) != 0 )
		{
			/*
			 * Call CFReadStreamRead one more time. This may block but this is
			 * the only way to know at termination if the download was
			 * finished, or if the download was incomplete.
			 */
			bytesRead = CFReadStreamRead(readStreamRecPtr->readStreamRef, buffer, 1); /* make it a small read */
			if ( bytesRead == 0 )
			{
				/*
				 * The download was complete the last time through the loop.
				 * Break and let the caller mark this download finished.
				 */
				break;
			}
			else
			{
				/*
				 * The download wasn't complete the last time through the loop.
				 * Throw out these bytes (we'll get them if the file is reopened)
				 * and let the caller mark this download aborted.
				 */
				goto terminated;
			}
		}
		
		bytesRead = CFReadStreamRead(readStreamRecPtr->readStreamRef, buffer, BODY_BUFFER_SIZE);
		if ( bytesRead > 0 )
		{
			require(write(node->file_fd, buffer, (size_t)bytesRead) == (ssize_t)bytesRead, write);
		}
		else if ( bytesRead == 0 )
		{
			/* there are no more bytes to read */
			break;
		}
		else
		{
			CFStreamError streamError;
			
			streamError = CFReadStreamGetError(readStreamRecPtr->readStreamRef);
			syslog(LOG_ERR,"network_finish_download: CFStreamError: domain %ld, error %ld", streamError.domain, streamError.error);
			goto CFReadStreamRead;
			break;
		}
	};

	free(buffer);

	if ( readStreamRecPtr->connectionClose )
	{
		/* close and release the stream */
		CFReadStreamClose(readStreamRecPtr->readStreamRef);
		CFRelease(readStreamRecPtr->readStreamRef);
		readStreamRecPtr->readStreamRef = NULL;
	}

	/* make this ReadStreamRec is available again */
	release_ReadStreamRec(readStreamRecPtr);
	
	return ( 0 );

terminated:
write:
CFReadStreamRead:

	free(buffer);

malloc_buffer:

	/* close and release the read stream on errors */
	CFReadStreamClose(readStreamRecPtr->readStreamRef);
	CFRelease(readStreamRecPtr->readStreamRef);
	readStreamRecPtr->readStreamRef = NULL;
	
	/* make this ReadStreamRec is available again */
	release_ReadStreamRec(readStreamRecPtr);

	return ( EIO );
}

/******************************************************************************/

int network_open(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> node to open */
	int write_access)			/* -> open requires write access */
{
	int error;
	int ask_server;
	
	if ( !write_access )
	{
		if ( ((node->file_status & WEBDAV_DOWNLOAD_STATUS_MASK) == WEBDAV_DOWNLOAD_FINISHED) && !NODE_FILE_INVALID(node) )
		{
			/* OK by our simple heuristics */
			/* the file was completely downloaded very recently, skip the server check */
			ask_server = FALSE;
		}
		else
		{
			/* attempt to retrieve file contents from the attributes_cache */
			if ( get_from_attributes_cache(node, uid) )
			{
				/* the file contents were retrieved from the attributes_cache */
				ask_server = FALSE;
			}
			else
			{
				/* we need to ask the server */
				ask_server = TRUE;
			}
		}
	}
	else
	{
		/* if we just created the file and it's completely downloaded, we won't check */
		if ( NODE_FILE_RECENTLY_CREATED(node) &&
			((node->file_status & WEBDAV_DOWNLOAD_STATUS_MASK) == WEBDAV_DOWNLOAD_FINISHED) )
		{
			ask_server = FALSE;
		}
		else
		{
			/* we must check with server when opening with write access */
			ask_server = TRUE;
		}
	}
	
	if ( ask_server )
	{
		CFURLRef urlRef;
		CFHTTPMessageRef message;
		CFHTTPMessageRef responseRef;
		UInt32 statusCode;
		UInt32 auth_generation;
		int retryTransaction;
				
		error = 0;
		message = NULL;
		responseRef = NULL;
		statusCode = 0;
		auth_generation = 0;
		retryTransaction = TRUE;

		/* create a CFURL to the node */
		urlRef = create_cfurl_from_node(node, NULL, 0);
		require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
		
		/* the transaction/authentication loop */
		do
		{
			/* release message if left from previous loop */
			if ( message != NULL )
			{
				CFRelease(message);
				message = NULL;
			}
			/* create a CFHTTP message object */
			message = CFHTTPMessageCreateRequest(kCFAllocatorDefault, CFSTR("GET"), urlRef, kCFHTTPVersion1_1);
			require_action(message != NULL, CFHTTPMessageCreateRequest, error = EIO);
			
			/* Change the User-Agent header */
			CFHTTPMessageSetHeaderFieldValue(message, CFSTR("User-Agent"), userAgentHeaderValue);
			
			/* add the X-Source-Id header if needed */
			if ( X_Source_Id_HeaderValue != NULL )
			{
				CFHTTPMessageSetHeaderFieldValue(message, CFSTR("X-Source-Id"), X_Source_Id_HeaderValue);
			}
			
			/* add other HTTP headers */
			CFHTTPMessageSetHeaderFieldValue(message, CFSTR("Accept"), CFSTR("*/*"));
			
			/*
			 * If the status isn't WEBDAV_DOWNLOAD_NEVER, we need to add some conditional headers.
			 * If adding the headers fails, then we can continue without them -- it'll just
			 * force the file to be downloaded.
			 */
			if ( (node->file_status & WEBDAV_DOWNLOAD_STATUS_MASK) != WEBDAV_DOWNLOAD_NEVER )
			{
				CFStringRef httpDateString;
				
				httpDateString = CFStringCreateRFC2616DateStringWithTimeT(node->file_last_modified);
				if ( httpDateString != NULL )
				{
					if ( (node->file_status & WEBDAV_DOWNLOAD_STATUS_MASK) == WEBDAV_DOWNLOAD_FINISHED )
					{
						CFHTTPMessageSetHeaderFieldValue(message, CFSTR("If-Modified-Since"), httpDateString);
					}
					else
					{
						off_t currentLength;
						CFStringRef currentLengthString;
						
						currentLength = lseek(node->file_fd, 0LL, SEEK_END);
						if ( currentLength != -1 )
						{
							/* create a string with the file length for the Range header */
							currentLengthString = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("bytes=%qd-"), currentLength);
							/* CFReadStreamCreateForStreamedHTTPRequest will use chunked transfer-encoding if the Content-Length header cannot be provided */
							if ( currentLengthString != NULL )
							{
								CFHTTPMessageSetHeaderFieldValue(message, CFSTR("If-Range"), httpDateString);
								CFHTTPMessageSetHeaderFieldValue(message, CFSTR("Range"), currentLengthString);
								CFRelease(currentLengthString);
							}
						}
					}
					CFRelease(httpDateString);
				}
			}
			
			/* apply credentials (if any) */
			/*
			 * statusCode will be 401 or 407 and responseRef will not be NULL if we've already been through the loop;
			 * statusCode will be 0 and responseRef will be NULL if this is the first time through.
			 */
			error = authcache_apply(uid, message, statusCode, responseRef, &auth_generation);
			if ( error != 0 )
			{
				break;
			}
			
			/* stream_transaction returns responseRef so release it if left from previous loop */
			if ( responseRef != NULL )
			{
				CFRelease(responseRef);
				responseRef = NULL;
			}
			/* now that everything's ready to send, send it */
			error = stream_get_transaction(message, &retryTransaction, node, &responseRef);
			if ( error == EAGAIN )
			{
				statusCode = 0;
				/* responseRef will be left NULL on retries */
			}
			else
			{
				if ( error != 0 )
				{
					break;
				}
				
				/* get the status code */
				statusCode = CFHTTPMessageGetResponseStatusCode(responseRef);
			}
		} while ( error == EAGAIN || statusCode == 401 || statusCode == 407 );

CFHTTPMessageCreateRequest:
		
		if ( error == 0 )
		{
			/* 304 Not Modified means the cache file is still good, so make it 200 before translating */
			if ( statusCode == 304 )
			{
				statusCode = 200;
			}
			
			error = translate_status_to_error(statusCode);
			if ( error == 0 )
			{
				/*
				 * when we get here with no errors, then we need to tell the authcache the
				 * transaction worked so it can mark the credentials valid and, if needed
				 * add the credentials to the keychain. If the auth_generation changed, then
				 * another transaction updated the authcache element after we got it.
				 */
				(void) authcache_valid(uid, message, auth_generation);
				time(&node->file_validated_time);
				{
					CFStringRef headerRef;
					const char *field_value;
					char buffer[4096];
					char *file_entity_tag;
					
					headerRef = CFHTTPMessageCopyHeaderFieldValue(responseRef, CFSTR("Last-Modified"));
					if ( headerRef )
					{
						node->file_last_modified = DateStringToTime(headerRef);
						CFRelease(headerRef);
					}
					headerRef = CFHTTPMessageCopyHeaderFieldValue(responseRef, CFSTR("ETag"));
					if ( headerRef )
					{
						field_value = CFStringGetCStringPtr(headerRef, kCFStringEncodingUTF8);
						if ( field_value == NULL )
						{
							if ( CFStringGetCString(headerRef, buffer, 4096, kCFStringEncodingUTF8) )
							{
								field_value = buffer;
							}
						}
						if ( field_value != NULL )
						{
							file_entity_tag = malloc(strlen(field_value) + 1);
							if ( file_entity_tag != NULL )
							{
								strcpy(file_entity_tag, field_value);
								if ( node->file_entity_tag != NULL )
								{
									free(node->file_entity_tag);
								}
								node->file_entity_tag = file_entity_tag;
							}
						}
						CFRelease(headerRef);
					}
				}
			}
		}
		
		if ( message != NULL )
		{
			CFRelease(message);
		}
		
		if ( responseRef != NULL )
		{
			CFRelease(responseRef);
		}
		
		CFRelease(urlRef);
		
create_cfurl_from_node:
		;
	}
	else
	{
		/* what we have cached is OK */
		error = 0;
	}

	return ( error );
}

/******************************************************************************/

int network_statfs(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> root node */
	struct statfs *fs_attr)		/* <- file system information */
{
	int error;
	CFURLRef urlRef;
	UInt8 *responseBuffer;
	CFIndex count;
	CFDataRef bodyData;
	const UInt8 xmlString[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<D:propfind xmlns:D=\"DAV:\">\n"
			"<D:prop>\n"
				"<D:quota/>\n"
				"<D:quotaused/>\n"
			"</D:prop>\n"
		"</D:propfind>\n";
	/* the 3 headers */
	CFIndex headerCount = 3;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("Content-Type"), CFSTR("text/xml") },
		{ CFSTR("Depth"), CFSTR("0") }
	};

	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
	
	/* create a CFDataRef with the xml that is our message body */
	bodyData = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, xmlString, strlen((const char *)xmlString), kCFAllocatorNull);
	require_action(bodyData != NULL, CFDataCreateWithBytesNoCopy, error = EIO);
	
	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("PROPFIND"), bodyData,
		headerCount, headers, TRUE, &responseBuffer, &count, NULL);
	if ( !error )
	{
		/* parse responseBuffer to get the statfsbuf */
		error = parse_statfs(responseBuffer, count, fs_attr);
		
		/* free the response buffer */
		free(responseBuffer);
	}
	
	/* release the message body */
	CFRelease(bodyData);
	
CFDataCreateWithBytesNoCopy:

	CFRelease(urlRef);
	
create_cfurl_from_node:
	
	return ( error );
}

/******************************************************************************/

int network_create(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> parent node */
	char *name,					/* -> file name to create */
	size_t name_length,			/* -> length of name */
	time_t *creation_date)		/* <- date of the creation */
{
	int error;
	CFURLRef urlRef;
	CFHTTPMessageRef response;
	/* the 1 header */
	CFIndex headerCount = 1;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") }
	};
	
	*creation_date = -1;

	/* create a CFURL to the node plus name */
	urlRef = create_cfurl_from_node(node, name, name_length);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);

	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("PUT"), NULL,
		headerCount, headers, FALSE, NULL, NULL, &response);
	if ( !error )
	{
		CFStringRef dateHeaderRef;
		
		dateHeaderRef = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("Date"));
		if ( dateHeaderRef != NULL )
		{
			*creation_date = DateStringToTime(dateHeaderRef);

			CFRelease(dateHeaderRef);
		}
		/* release the response buffer */
		CFRelease(response);
	}
	
	CFRelease(urlRef);

create_cfurl_from_node:
	
	return ( error );
}

/******************************************************************************/

int network_fsync(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> node to sync with server */
	off_t *file_length,			/* <- length of file */
	time_t *file_last_modified)	/* <- date of last modification */
{
	int error;
	CFURLRef urlRef;
	CFHTTPMessageRef message;
	CFHTTPMessageRef responseRef;
	UInt32 statusCode;
	UInt32 auth_generation;
	CFStringRef lockTokenRef;
	char *file_entity_tag;
	int retryTransaction;
	
	error = 0;
	*file_last_modified = -1;
	*file_length = -1;
	file_entity_tag = NULL;
	message = NULL;
	responseRef = NULL;
	statusCode = 0;
	auth_generation = 0;
	retryTransaction = TRUE;
	
	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
	
	/* the transaction/authentication loop */
	do
	{
		/* release message if left from previous loop */
		if ( message != NULL )
		{
			CFRelease(message);
			message = NULL;
		}
		/* create a CFHTTP message object */
		message = CFHTTPMessageCreateRequest(kCFAllocatorDefault, CFSTR("PUT"), urlRef, kCFHTTPVersion1_1);
		require_action(message != NULL, CFHTTPMessageCreateRequest, error = EIO);
		
		/* Change the User-Agent header */
		CFHTTPMessageSetHeaderFieldValue(message, CFSTR("User-Agent"), userAgentHeaderValue);
		
		/* add the X-Source-Id header if needed */
		if ( X_Source_Id_HeaderValue != NULL )
		{
			CFHTTPMessageSetHeaderFieldValue(message, CFSTR("X-Source-Id"), X_Source_Id_HeaderValue);
		}
		
		/* add other HTTP headers */
		CFHTTPMessageSetHeaderFieldValue(message, CFSTR("Accept"), CFSTR("*/*"));
		
		/* is there a lock token? */
		if ( node->file_locktoken != NULL )
		{
			/* in the unlikely event that this fails, the PUT may fail */
			lockTokenRef = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("(<%s>)"), node->file_locktoken);
			if ( lockTokenRef != NULL )
			{
				CFHTTPMessageSetHeaderFieldValue(message, CFSTR("If"), lockTokenRef );
				CFRelease(lockTokenRef);
				lockTokenRef = NULL;
			}
		}
		else
		{
			lockTokenRef = NULL;
		}
		
		/* apply credentials (if any) */
		/*
		 * statusCode will be 401 or 407 and responseRef will not be NULL if we've already been through the loop;
		 * statusCode will be 0 and responseRef will be NULL if this is the first time through.
		 */
		error = authcache_apply(uid, message, statusCode, responseRef, &auth_generation);
		if ( error != 0 )
		{
			break;
		}
		
		/* stream_transaction returns responseRef so release it if left from previous loop */
		if ( responseRef != NULL )
		{
			CFRelease(responseRef);
			responseRef = NULL;
		}
		/* now that everything's ready to send, send it */
		
		error = stream_transaction_from_file(message, node->file_fd, &retryTransaction, &responseRef);
		if ( error == EAGAIN )
		{
			statusCode = 0;
			/* responseRef will be left NULL on retries */
		}
		else
		{
			if ( error != 0 )
			{
				break;
			}
			
			/* get the status code */
			statusCode = CFHTTPMessageGetResponseStatusCode(responseRef);
		}
	} while ( error == EAGAIN || statusCode == 401 || statusCode == 407 );

CFHTTPMessageCreateRequest:

	if ( error == 0 )
	{
		error = translate_status_to_error(statusCode);
		if ( error == 0 )
		{
			/*
			 * when we get here with no errors, then we need to tell the authcache the
			 * transaction worked so it can mark the credentials valid and, if needed
			 * add the credentials to the keychain. If the auth_generation changed, then
			 * another transaction updated the authcache element after we got it.
			 */
			(void) authcache_valid(uid, message, auth_generation);
			{
				CFStringRef headerRef;
				const char *field_value;
				char buffer[4096];
				
				headerRef = CFHTTPMessageCopyHeaderFieldValue(responseRef, CFSTR("Last-Modified"));
				if ( headerRef )
				{
					*file_last_modified = DateStringToTime(headerRef);
					CFRelease(headerRef);
				}
				headerRef = CFHTTPMessageCopyHeaderFieldValue(responseRef, CFSTR("ETag"));
				if ( headerRef )
				{
					field_value = CFStringGetCStringPtr(headerRef, kCFStringEncodingUTF8);
					if ( field_value == NULL )
					{
						if ( CFStringGetCString(headerRef, buffer, 4096, kCFStringEncodingUTF8) )
						{
							field_value = buffer;
						}
					}
					if ( field_value != NULL )
					{
						file_entity_tag = malloc(strlen(field_value) + 1);
						if ( file_entity_tag != NULL )
						{
							strcpy(file_entity_tag, field_value);
						}
					}
					CFRelease(headerRef);
				}
			}
		}
	}
	
	if ( message != NULL )
	{
		CFRelease(message);
	}
	
	if ( responseRef != NULL )
	{
		CFRelease(responseRef);
	}
	
	if ( (error == 0) && (*file_last_modified == -1) && (file_entity_tag == NULL) )
	{
		int propError;
		UInt8 *responseBuffer;
		CFIndex count;
		CFDataRef bodyData;
		/* the xml for the message body */
		const UInt8 xmlString[] =
			"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
			"<D:propfind xmlns:D=\"DAV:\">\n"
				"<D:prop>\n"
					"<D:getlastmodified/>\n"
					"<D:getetag/>\n"
				"</D:prop>\n"
			"</D:propfind>\n";
		/* the 3 headers */
		CFIndex headerCount = 3;
		struct HeaderFieldValue headers[] = {
			{ CFSTR("Accept"), CFSTR("*/*") },
			{ CFSTR("Content-Type"), CFSTR("text/xml") },
			{ CFSTR("Depth"), CFSTR("0") }
		};

		propError = 0;
		responseBuffer = NULL;

		/* create the message body with the xml */
		bodyData = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, xmlString, strlen((const char *)xmlString), kCFAllocatorNull);
		require_action(bodyData != NULL, CFDataCreateWithBytesNoCopy, propError = EIO);
		
		/* send request to the server and get the response */
		propError = send_transaction(uid, urlRef, CFSTR("PROPFIND"), bodyData,
			headerCount, headers, TRUE, &responseBuffer, &count, NULL);
		if ( propError == 0 )
		{
			/* parse responseBuffer to get file_last_modified and/or file_entity_tag */
			propError = parse_cachevalidators(responseBuffer, count, file_last_modified, &file_entity_tag);
			/* free the response buffer */
			free(responseBuffer);
		}
		
		/* release the message body */
		CFRelease(bodyData);
		
CFDataCreateWithBytesNoCopy:
		;
	}
	
	CFRelease(urlRef);

create_cfurl_from_node:
	
	if ( !error )
	{
		node->file_last_modified = *file_last_modified;
		if ( node->file_entity_tag != NULL )
		{
			free(node->file_entity_tag);
		}
		node->file_entity_tag = file_entity_tag;
		
		/* get the file length */
		*file_length = lseek(node->file_fd, 0LL, SEEK_END);
	}

	return ( error );
}

/******************************************************************************/

static int network_delete(
	uid_t uid,					/* -> uid of the user making the request */
	CFURLRef urlRef,			/* -> url to delete */
	struct node_entry *node,	/* -> node to remove on the server */
	time_t *remove_date)		/* <- date of the removal */
{
	int error;
	CFStringRef lockTokenRef;
	CFHTTPMessageRef response;
	/* possibly 2 headers */
	CFIndex headerCount;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("If"), NULL }
	};

	*remove_date = -1;

	if ( node->file_locktoken != NULL )
	{
		/* in the unlikely event that this fails, the DELETE will fail */
		lockTokenRef = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("(<%s>)"), node->file_locktoken);
		if ( lockTokenRef != NULL )
		{
			headerCount = 2;
			headers[1].value = lockTokenRef;
		}
		else
		{
			headerCount = 1;
		}
	}
	else
	{
		lockTokenRef = NULL;
		headerCount = 1;
	}

	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("DELETE"), NULL,
		headerCount, headers, FALSE, NULL, NULL, &response);
	if ( !error )
	{
		CFStringRef dateHeaderRef;
		
		dateHeaderRef = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("Date"));
		if ( dateHeaderRef != NULL )
		{
			*remove_date = DateStringToTime(dateHeaderRef);

			CFRelease(dateHeaderRef);
		}
		/* release the response buffer */
		CFRelease(response);
	}
	
	if ( lockTokenRef != NULL )
	{
		CFRelease(lockTokenRef);
	}
	
	return ( error );
}

/******************************************************************************/

int network_remove(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> file node to remove on the server */
	time_t *remove_date)		/* <- date of the removal */
{
	int error;
	CFURLRef urlRef;
	
	error = 0;
	
	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
	
	/* let network_delete do the rest of the work */
	error = network_delete(uid, urlRef, node, remove_date);
	
	CFRelease(urlRef);

create_cfurl_from_node:

	return ( error );
}

/******************************************************************************/

int network_rmdir(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> directory node to remove on the server */
	time_t *remove_date)		/* <- date of the removal */
{
	int error;
	CFURLRef urlRef;
	
	error = 0;
	
	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
	
	/* make sure the directory is empty */
	error = network_dir_is_empty(uid, urlRef);
	if ( !error )
	{
		/* let network_delete do the rest of the work */
		error = network_delete(uid, urlRef, node, remove_date);
	}
	
	CFRelease(urlRef);

create_cfurl_from_node:

	return ( error );
}

/******************************************************************************/

/* NOTE: this will call network_dir_is_empty() if to_node is a valid and a directory.
 */
int network_rename(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *from_node, /* node to move */
	struct node_entry *to_node,	/* node to move over (ignored if NULL) */
	struct node_entry *to_dir_node, /* directory node move into (ignored if to_node != NULL) */
	char *to_name,				/* new name for the object (ignored if to_node != NULL) */
	size_t to_name_length,		/* length of to_name (ignored if to_node != NULL) */
	time_t *rename_date)		/* <- date of the rename */
{
	int error;
	CFURLRef urlRef;
	CFURLRef destinationUrlRef;
	CFStringRef destinationRef;
	CFHTTPMessageRef response;
	/* the 2 headers */
	CFIndex headerCount = 2;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("Destination"), NULL }
	};
	
	*rename_date = -1;
	urlRef = NULL;
	destinationUrlRef = NULL;
	destinationRef = NULL;
	
	/* create a CFURL to the from_node */
	urlRef = create_cfurl_from_node(from_node, NULL, 0);
	require_action_quiet(urlRef != NULL, exit, error = EIO);

	/* create the URL for the destination */
	if ( to_node != NULL )
	{
		/* use to_node */
		
		/* create a CFURL to the to_node */
		destinationUrlRef = create_cfurl_from_node(to_node, NULL, 0);
		require_action_quiet(destinationUrlRef != NULL, exit, error = EIO);
		
		/* if source and destination are equal, there's nothing to do so leave with no error */
		require_action_quiet( !CFEqual(urlRef, destinationUrlRef), exit, error = 0);

		/* is the destination a directory? */
		if ( to_node->node_type == WEBDAV_DIR_TYPE )
		{
			/* make sure the directory is empty before attempting to move over it */
			error = network_dir_is_empty(uid, destinationUrlRef);
			require_noerr_quiet(error, exit);
		}
	}
	else
	{
		/* use to_dir_node and to_name */
		
		/* create a CFURL to the to_dir_node plus name */
		destinationUrlRef = create_cfurl_from_node(to_dir_node, to_name, to_name_length);
		require_action_quiet(destinationUrlRef != NULL, exit, error = EIO);

		/* if source and destination are equal, there's nothing to do so leave with no error */
		require_action_quiet( !CFEqual(urlRef, destinationUrlRef), exit, error = 0);
	}
	
	destinationRef = CFURLGetString(destinationUrlRef);
	require_action(destinationRef != NULL, exit, error = EIO);
	
	headers[1].value = destinationRef; /* done with that mess... */
	
	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("MOVE"), NULL,
		headerCount, headers, FALSE, NULL, NULL, &response);
	if ( !error )
	{
		CFStringRef dateHeaderRef;
		
		dateHeaderRef = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("Date"));
		if ( dateHeaderRef != NULL )
		{
			*rename_date = DateStringToTime(dateHeaderRef);

			CFRelease(dateHeaderRef);
		}
		/* release the response buffer */
		CFRelease(response);
	}

exit:
	
	if ( destinationUrlRef != NULL )
	{
		CFRelease(destinationUrlRef);
	}
	if ( urlRef != NULL )
	{
		CFRelease(urlRef);
	}
	
	return ( error );
}

/******************************************************************************/

int network_lock(
	uid_t uid,					/* -> uid of the user making the request (ignored if refreshing) */
	int refresh,				/* -> if FALSE, we're getting the lock (for uid); if TRUE, we're refreshing the lock */
	struct node_entry *node)	/* -> node to get/renew server lock on */
{
	int error;
	CFURLRef urlRef;
	UInt8 *responseBuffer;
	CFIndex count;
	CFDataRef bodyData;
	const UInt8 xmlString[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<D:lockinfo xmlns:D=\"DAV:\">\n"
			"<D:lockscope><D:exclusive/></D:lockscope>\n"
			"<D:locktype><D:write/></D:locktype>\n"
			"<D:owner>\n"
				"<D:href>http://www.apple.com/webdav_fs/</D:href>\n" /* this used to be "default-owner" instead of the url */
			"</D:owner>\n"
		"</D:lockinfo>\n";
	/* the 3 headers */
	CFIndex headerCount = 4;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("Depth"), CFSTR("0") },
		{ CFSTR("Timeout"), NULL },
		{ CFSTR("Content-Type"), NULL },
		{ CFSTR("If"), NULL }
	};
	CFStringRef timeoutSpecifierRef;
	CFStringRef lockTokenRef;

	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
	
	timeoutSpecifierRef = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("Second-%s"), gtimeout_string);
	require_action(timeoutSpecifierRef != NULL, CFStringCreateWithFormat_timeoutSpecifierRef, error = EIO);
	
	headers[2].value = timeoutSpecifierRef;
	
	if ( refresh )
	{
		/* if refreshing, use the uid associated with the file_locktoken */
		uid = node->file_locktoken_uid;
		
		/* if refreshing the lock, there's no message body */
		bodyData = NULL;
		
		headerCount = 5;
		headers[3].value = CFSTR("text/xml");
		lockTokenRef = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("(<%s>)"), node->file_locktoken);
		require_action(lockTokenRef != NULL, CFStringCreateWithFormat_lockTokenRef, error = EIO);
		
		headers[4].value = lockTokenRef;
	}
	else
	{
		lockTokenRef = NULL;
		/* create a CFDataRef with the xml that is our message body */
		bodyData = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, xmlString, strlen((const char *)xmlString), kCFAllocatorNull);
		require_action(bodyData != NULL, CFDataCreateWithBytesNoCopy, error = EIO);
		
		headerCount = 4;
		headers[3].value = CFSTR("text/xml; charset=\"utf-8\"");
	}
	
	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("LOCK"), bodyData,
		headerCount, headers, FALSE, &responseBuffer, &count, NULL);
	if ( !error )
	{
		char *locktoken;

		/* parse responseBuffer to get the lock token */
		error = parse_lock(responseBuffer, count, &locktoken);
		
		if ( !error )
		{
			char *old_locktoken;
			
			old_locktoken = node->file_locktoken;
			node->file_locktoken = locktoken;
			if ( old_locktoken != NULL )
			{
				
				free(old_locktoken);
			}
			/* file_locktoken_uid is already set if refreshing */
			if ( !refresh )
			{
				node->file_locktoken_uid = uid;
			}
		}
		
		/* free the response buffer */
		free(responseBuffer);
	}

	if ( bodyData != NULL )
	{
		CFRelease(bodyData);
	}
	
	if ( lockTokenRef != NULL )
	{
		CFRelease(lockTokenRef);
	}

CFDataCreateWithBytesNoCopy:
CFStringCreateWithFormat_lockTokenRef:

	CFRelease(timeoutSpecifierRef);

CFStringCreateWithFormat_timeoutSpecifierRef:

	CFRelease(urlRef);

create_cfurl_from_node:
	
	return ( error );
}

/******************************************************************************/

int network_unlock(
	struct node_entry *node)	/* -> node to unlock on server */
{
	int error;
	CFURLRef urlRef;
	CFStringRef lockTokenRef;
	/* the 2 headers */
	CFIndex headerCount = 2;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("Lock-Token"), NULL }
	};

	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);
	
	/* in the unlikely event that this fails, the DELETE will fail */
	lockTokenRef = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<%s>"), node->file_locktoken);
	require_action_quiet(lockTokenRef != NULL, CFStringCreateWithFormat, error = EIO);

	headers[1].value = lockTokenRef;
	
	/* send request to the server and get the response */
	/* Note: we use the credentials of the user than obtained the LOCK */
	error = send_transaction(node->file_locktoken_uid, urlRef, CFSTR("UNLOCK"), NULL,
		headerCount, headers, FALSE, NULL, NULL, NULL);
	
	CFRelease(lockTokenRef);

CFStringCreateWithFormat:
	
	CFRelease(urlRef);

create_cfurl_from_node:
	
	free(node->file_locktoken);
	node->file_locktoken_uid = 0;
	node->file_locktoken = NULL;
	
	return ( error );
}

/******************************************************************************/

int network_readdir(
	uid_t uid,					/* -> uid of the user making the request */
	int cache,					/* -> if TRUE, perform additional caching */
	struct node_entry *node)	/* -> directory node to read */
{
	int error;
	CFURLRef urlRef;
	UInt8 *responseBuffer;
	CFIndex count;
	CFDataRef bodyData;
	const UInt8 xmlString[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<D:propfind xmlns:D=\"DAV:\">\n"
			"<D:prop>\n"
				"<D:getlastmodified/>\n"
				"<D:getcontentlength/>\n"
				"<D:resourcetype/>\n"
			"</D:prop>\n"
		"</D:propfind>\n";
	const UInt8 xmlStringCache[] =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		"<D:propfind xmlns:D=\"DAV:\">\n"
			"<D:prop xmlns:A=\"http://www.apple.com/webdav_fs/props/\">\n"
				"<D:getlastmodified/>\n"
				"<D:getcontentlength/>\n"
				"<D:resourcetype/>\n"
				"<A:appledoubleheader/>\n"
			"</D:prop>\n"
		"</D:propfind>\n";
	/* the 3 headers */
	CFIndex headerCount = 3;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("Content-Type"), CFSTR("text/xml") },
		{ CFSTR("Depth"), CFSTR("1") }
	};

	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);

	/* create a CFDataRef with the xml that is our message body */
	bodyData = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
		(cache ? xmlStringCache : xmlString), strlen((const char *)(cache ? xmlStringCache : xmlString)), kCFAllocatorNull);
	require_action(bodyData != NULL, CFDataCreateWithBytesNoCopy, error = EIO);

	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("PROPFIND"), bodyData,
		headerCount, headers, TRUE, &responseBuffer, &count, NULL);
	if ( !error )
	{
		/* parse responseBuffer to create the directory file */
		error = parse_opendir(responseBuffer, count, urlRef, uid, node);
		/* free the response buffer */
		free(responseBuffer);
	}
	
	/* release the message body */
	CFRelease(bodyData);

CFDataCreateWithBytesNoCopy:

	CFRelease(urlRef);

create_cfurl_from_node:

	return ( error );
}

/******************************************************************************/

int network_mkdir(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> parent node */
	char *name,					/* -> directory name to create */
	size_t name_length,			/* -> length of name */
	time_t *creation_date)		/* <- date of the creation */
{
	int error;
	CFURLRef urlRef;
	CFHTTPMessageRef response;
	/* the 3 headers */
	CFIndex headerCount = 1;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
	};

	*creation_date = -1;

	/* create a CFURL to the node plus name */
	urlRef = create_cfurl_from_node(node, name, name_length);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);

	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("MKCOL"), NULL,
		headerCount, headers, FALSE, NULL, NULL, &response);
	if ( !error )
	{
		CFStringRef dateHeaderRef;
		
		dateHeaderRef = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("Date"));
		if ( dateHeaderRef != NULL )
		{
			*creation_date = DateStringToTime(dateHeaderRef);

			CFRelease(dateHeaderRef);
		}
		/* release the response buffer */
		CFRelease(response);
	}
	
	CFRelease(urlRef);

create_cfurl_from_node:

	return ( error );
}

/******************************************************************************/

int network_read(
	uid_t uid,					/* -> uid of the user making the request */
	struct node_entry *node,	/* -> node to read */
	off_t offset,				/* -> position within the file at which the read is to begin */
	size_t count,				/* -> number of bytes of data to be read */
	char **buffer,				/* <- buffer data was read into (allocated by network_read) */
	size_t *actual_count)		/* <- number of bytes actually read */
{
	int error;
	CFURLRef urlRef;
	UInt8 *responseBuffer;
	CFIndex responseCount;
	CFStringRef byteRangesSpecifierRef;
	/* the 2 headers -- the range value will be computed below */
	CFIndex headerCount = 2;
	struct HeaderFieldValue headers[] = {
		{ CFSTR("Accept"), CFSTR("*/*") },
		{ CFSTR("Range"), NULL }
	};
	
	*buffer = NULL;
	*actual_count = 0;
	
	/* create a CFURL to the node */
	urlRef = create_cfurl_from_node(node, NULL, 0);
	require_action_quiet(urlRef != NULL, create_cfurl_from_node, error = EIO);

	byteRangesSpecifierRef = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("bytes=%qd-%qd"), offset, offset + count - 1);
	require_action(byteRangesSpecifierRef != NULL, CFStringCreateWithFormat, error = EIO);

	headers[1].value = byteRangesSpecifierRef;
	
	/* send request to the server and get the response */
	error = send_transaction(uid, urlRef, CFSTR("GET"), NULL,
		headerCount, headers, TRUE, &responseBuffer, &responseCount, NULL);
	if ( !error )
	{
		if ( (size_t)responseCount > count )
		{
			/* don't return more than we asked for */
			responseCount = count;
		}
		*buffer = (char *)responseBuffer;
		*actual_count = responseCount;
	}
	
	CFRelease(byteRangesSpecifierRef);
	
CFStringCreateWithFormat:

	CFRelease(urlRef);
	
create_cfurl_from_node:

	return ( error );
}

/******************************************************************************/
