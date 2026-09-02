#pragma once
#include <string>
#include <utility>
#include <vector>

#include "types.h"

// A deliberately narrow HTTP client: GET and POST, headers, a body, a status
// code and a timeout. Nothing more.
//
// Odin needs this for exactly two things - probing provider endpoints in
// `doctor`, and the built-in OpenAI-compatible adapter - so a provider SDK would
// be a large dependency for a small surface. TLS is not implemented here either;
// it is delegated to the platform:
//
//   Windows        WinHTTP, using the system certificate and proxy stack
//   everything else libcurl
//
// That split is why there is no bundled CA file to ship or keep current on
// Windows, and why proxy configuration works without Odin knowing about proxies.

struct http_request
{
	std::string method = "GET";
	std::string url;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
	int timeout_seconds = 30;
};

struct http_response
{
	int status = 0;
	std::string body;
};

// Returns false only when the exchange could not happen at all: DNS failure, a
// refused connection, a TLS error, a timeout.
//
// A 4xx or 5xx is a SUCCESSFUL call carrying that status, and out_response.body
// holds the error body - the same distinction subprocess_run makes between "did
// not run" and "ran and failed". Both callers need the error body: the adapter
// puts it in a diagnostic, and discovery distinguishes 401/403 from the rest.
bool http_send(const http_request &request, http_response &out_response, odin_error &out_error);

// A cheap TCP pre-connect, so a closed port costs milliseconds instead of the
// full HTTP timeout.
//
// Without it `doctor` waits out a timeout per unused local port and takes long
// enough that no GUI would call it on demand - which is the whole point of the
// command.
bool http_port_open(const std::string &host, int port, int timeout_ms);
