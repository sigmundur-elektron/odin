#include "http_client.h"

#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// winsock2 must precede windows.h's implicit winsock1; WIN32_LEAN_AND_MEAN
// above is what keeps the two from colliding.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <curl/curl.h>

#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace
{

#ifdef _WIN32

std::wstring widen(const std::string &utf8)
{
	if (utf8.empty())
		return {};
	const int size =
	  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring wide(static_cast<std::size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
	return wide;
}

// Explicit teardown rather than a destructor: this codebase keeps handle
// lifetimes visible at the call site, so every exit path calls this.
void winhttp_close(HINTERNET session, HINTERNET connection, HINTERNET request)
{
	if (request != nullptr)
		WinHttpCloseHandle(request);
	if (connection != nullptr)
		WinHttpCloseHandle(connection);
	if (session != nullptr)
		WinHttpCloseHandle(session);
}

std::string last_error_text(const char *what)
{
	return std::string(what) + " failed (winhttp error " +
		   std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
}

#else

std::size_t curl_collect(char *data, std::size_t size, std::size_t count, void *sink)
{
	const std::size_t total = size * count;
	static_cast<std::string *>(sink)->append(data, total);
	return total;
}

#endif

} // namespace

#ifdef _WIN32

bool http_send(const http_request &request, http_response &out_response, odin_error &out_error)
{
	const std::wstring url = widen(request.url);

	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	parts.dwSchemeLength = static_cast<DWORD>(-1);
	parts.dwHostNameLength = static_cast<DWORD>(-1);
	parts.dwUrlPathLength = static_cast<DWORD>(-1);
	parts.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
	{
		fail(out_error, error_kind::config, "could not parse URL: " + request.url);
		return false;
	}

	const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
	std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
	if (parts.dwExtraInfoLength > 0)
		target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
	const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

	// WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY picks up the machine's proxy
	// configuration, which is the main reason to use WinHTTP over rolling
	// sockets: a corporate proxy needs no configuration in odin.toml.
	HINTERNET session = WinHttpOpen(L"odin", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
									WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (session == nullptr)
	{
		fail(out_error, error_kind::config, last_error_text("WinHttpOpen"));
		return false;
	}

	const int milliseconds = request.timeout_seconds * 1000;
	WinHttpSetTimeouts(session, milliseconds, milliseconds, milliseconds, milliseconds);

	HINTERNET connection = WinHttpConnect(session, host.c_str(),
										  static_cast<INTERNET_PORT>(parts.nPort), 0);
	if (connection == nullptr)
	{
		const std::string message = last_error_text("WinHttpConnect");
		winhttp_close(session, nullptr, nullptr);
		fail(out_error, error_kind::config, message);
		return false;
	}

	HINTERNET exchange = WinHttpOpenRequest(connection, widen(request.method).c_str(),
											target.c_str(), nullptr, WINHTTP_NO_REFERER,
											WINHTTP_DEFAULT_ACCEPT_TYPES,
											secure ? WINHTTP_FLAG_SECURE : 0);
	if (exchange == nullptr)
	{
		const std::string message = last_error_text("WinHttpOpenRequest");
		winhttp_close(session, connection, nullptr);
		fail(out_error, error_kind::config, message);
		return false;
	}

	std::wstring headers;
	for (const auto &[name, value] : request.headers)
		headers += widen(name + ": " + value + "\r\n");
	if (!headers.empty())
		WinHttpAddRequestHeaders(exchange, headers.c_str(), static_cast<DWORD>(headers.size()),
								 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

	const BOOL sent = WinHttpSendRequest(
	  exchange, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
	  request.body.empty() ? WINHTTP_NO_REQUEST_DATA
						   : const_cast<char *>(request.body.data()),
	  static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
	if (!sent || !WinHttpReceiveResponse(exchange, nullptr))
	{
		const std::string message = last_error_text("request");
		winhttp_close(session, connection, exchange);
		fail(out_error, error_kind::config, message);
		return false;
	}

	DWORD status = 0;
	DWORD status_size = sizeof(status);
	WinHttpQueryHeaders(exchange, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
						WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
	out_response.status = static_cast<int>(status);

	out_response.body.clear();
	for (;;)
	{
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(exchange, &available) || available == 0)
			break;
		std::string chunk(available, '\0');
		DWORD read = 0;
		if (!WinHttpReadData(exchange, chunk.data(), available, &read) || read == 0)
			break;
		out_response.body.append(chunk, 0, read);
	}

	winhttp_close(session, connection, exchange);
	return true;
}

bool http_port_open(const std::string &host, int port, int timeout_ms)
{
	WSADATA winsock = {};
	if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
		return false;

	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo *resolved = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &resolved) != 0)
	{
		WSACleanup();
		return false;
	}

	bool open = false;
	for (addrinfo *entry = resolved; entry != nullptr && !open; entry = entry->ai_next)
	{
		const SOCKET handle = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
		if (handle == INVALID_SOCKET)
			continue;

		// non-blocking connect plus select, so the wait is bounded by the
		// caller's timeout rather than by the OS default of ~20 seconds
		u_long non_blocking = 1;
		ioctlsocket(handle, FIONBIO, &non_blocking);

		const int result = connect(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
		if (result == 0)
		{
			open = true;
		}
		else if (WSAGetLastError() == WSAEWOULDBLOCK)
		{
			// the exception set is not optional on Windows. a refused
			// connection signals there, not in the write set, so watching
			// writes alone would wait out the whole timeout for every closed
			// port instead of failing immediately - which is exactly the cost
			// this pre-check exists to avoid.
			fd_set writable;
			fd_set failed_set;
			FD_ZERO(&writable);
			FD_ZERO(&failed_set);
			FD_SET(handle, &writable);
			FD_SET(handle, &failed_set);
			timeval limit = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
			if (select(0, nullptr, &writable, &failed_set, &limit) > 0 &&
				FD_ISSET(handle, &writable))
			{
				int error = 0;
				int size = sizeof(error);
				if (getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error),
							   &size) == 0 &&
					error == 0)
					open = true;
			}
		}
		closesocket(handle);
	}

	freeaddrinfo(resolved);
	WSACleanup();
	return open;
}

#else

bool http_send(const http_request &request, http_response &out_response, odin_error &out_error)
{
	CURL *handle = curl_easy_init();
	if (handle == nullptr)
	{
		fail(out_error, error_kind::config, "could not initialise the HTTP client");
		return false;
	}

	out_response.body.clear();

	curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
	curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(request.timeout_seconds));
	curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(handle, CURLOPT_USERAGENT, "odin");
	curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_collect);
	curl_easy_setopt(handle, CURLOPT_WRITEDATA, &out_response.body);
	// the body of a non-2xx is wanted, not discarded: it carries the provider's
	// explanation, and discovery needs the status rather than an error
	curl_easy_setopt(handle, CURLOPT_FAILONERROR, 0L);

	if (request.method == "POST")
	{
		curl_easy_setopt(handle, CURLOPT_POST, 1L);
		curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
		curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
						 static_cast<long>(request.body.size()));
	}

	curl_slist *headers = nullptr;
	for (const auto &[name, value] : request.headers)
		headers = curl_slist_append(headers, (name + ": " + value).c_str());
	if (headers != nullptr)
		curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

	const CURLcode code = curl_easy_perform(handle);

	if (code != CURLE_OK)
	{
		const std::string message = curl_easy_strerror(code);
		if (headers != nullptr)
			curl_slist_free_all(headers);
		curl_easy_cleanup(handle);
		fail(out_error, error_kind::config, message);
		return false;
	}

	long status = 0;
	curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
	out_response.status = static_cast<int>(status);

	if (headers != nullptr)
		curl_slist_free_all(headers);
	curl_easy_cleanup(handle);
	return true;
}

bool http_port_open(const std::string &host, int port, int timeout_ms)
{
	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo *resolved = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &resolved) != 0)
		return false;

	bool open = false;
	for (addrinfo *entry = resolved; entry != nullptr && !open; entry = entry->ai_next)
	{
		const int handle = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
		if (handle < 0)
			continue;

		const int flags = fcntl(handle, F_GETFL, 0);
		fcntl(handle, F_SETFL, flags | O_NONBLOCK);

		const int result = connect(handle, entry->ai_addr, entry->ai_addrlen);
		if (result == 0)
		{
			open = true;
		}
		else if (errno == EINPROGRESS)
		{
			fd_set writable;
			FD_ZERO(&writable);
			FD_SET(handle, &writable);
			timeval limit = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
			if (select(handle + 1, nullptr, &writable, nullptr, &limit) > 0)
			{
				int error = 0;
				socklen_t size = sizeof(error);
				if (getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0)
					open = true;
			}
		}
		close(handle);
	}

	freeaddrinfo(resolved);
	return open;
}

#endif
