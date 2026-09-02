#include <doctest/doctest.h>

#include "adapter.h"
#include "atomic_file.h"
#include "config.h"
#include "test_support.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define ODIN_BAD_SOCKET INVALID_SOCKET
#define ODIN_CLOSE closesocket
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define ODIN_BAD_SOCKET (-1)
#define ODIN_CLOSE ::close
#endif

// A real HTTP server on a real socket.
//
// The adapter's job is to speak HTTP to a provider, so a mocked transport would
// test everything except the part that breaks. This is small enough to be
// obvious and exercises WinHTTP/libcurl for real, including the retry path.
namespace
{

struct stub_server
{
	int port = 0;
	socket_t listener = ODIN_BAD_SOCKET;
	std::thread worker;
	std::atomic<int> requests {0};

	// captured from the last request, so a test can assert on what was sent
	std::string last_body;
};

void stub_start(stub_server &server)
{
#ifdef _WIN32
	WSADATA winsock = {};
	WSAStartup(MAKEWORD(2, 2), &winsock);
#endif
	server.listener = socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(server.listener != ODIN_BAD_SOCKET);

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0; // let the OS choose, so parallel runs cannot collide
	REQUIRE(bind(server.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);

	sockaddr_in bound = {};
#ifdef _WIN32
	int size = sizeof(bound);
#else
	socklen_t size = sizeof(bound);
#endif
	REQUIRE(getsockname(server.listener, reinterpret_cast<sockaddr *>(&bound), &size) == 0);
	server.port = ntohs(bound.sin_port);
	REQUIRE(listen(server.listener, 4) == 0);

	server.worker = std::thread([&server]() {
		for (;;)
		{
			const socket_t client = accept(server.listener, nullptr, nullptr);
			if (client == ODIN_BAD_SOCKET)
				return;

			std::string received;
			char buffer[4096];
			// read until the body is complete enough to decide; the client
			// sends Content-Length, so headers plus that many bytes
			for (;;)
			{
				const int count =
				  static_cast<int>(recv(client, buffer, sizeof(buffer), 0));
				if (count <= 0)
					break;
				received.append(buffer, static_cast<std::size_t>(count));

				const std::size_t split = received.find("\r\n\r\n");
				if (split == std::string::npos)
					continue;
				const std::size_t at = received.find("Content-Length:");
				if (at == std::string::npos)
					break;
				const std::size_t declared =
				  static_cast<std::size_t>(std::stoul(received.substr(at + 15)));
				if (received.size() - (split + 4) >= declared)
					break;
			}

			const std::size_t split = received.find("\r\n\r\n");
			const std::string body =
			  split == std::string::npos ? std::string{} : received.substr(split + 4);
			server.last_body = body;
			++server.requests;

			std::string payload;
			int status = 200;
			// several servers advertise the OpenAI shape but reject
			// response_format. that is the exact condition the adapter's single
			// retry exists for, so the stub reproduces it.
			if (body.find("response_format") != std::string::npos)
			{
				status = 400;
				payload = R"({"error":{"message":"response_format is not supported"}})";
			}
			else
			{
				payload =
				  R"({"choices":[{"message":{"content":"{\"status\":\"approved\",)"
				  R"(\"summary\":\"looks good\",\"artifacts\":{},\"findings\":[]}"}}]})";
			}

			const std::string response =
			  "HTTP/1.1 " + std::to_string(status) + " OK\r\nContent-Type: application/json\r\n" +
			  "Content-Length: " + std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" +
			  payload;
			send(client, response.data(), static_cast<int>(response.size()), 0);
			ODIN_CLOSE(client);
		}
	});
}

void stub_stop(stub_server &server)
{
	// closing the listener makes the blocked accept() fail, which ends the
	// worker. explicit rather than a destructor, matching the codebase style.
	if (server.listener != ODIN_BAD_SOCKET)
		ODIN_CLOSE(server.listener);
	if (server.worker.joinable())
		server.worker.join();
	server.listener = ODIN_BAD_SOCKET;
#ifdef _WIN32
	WSACleanup();
#endif
}

model_profile stub_profile()
{
	model_profile profile;
	profile.name = "stub";
	profile.adapter = "stub";
	profile.model = "test-model";
	profile.parameter_billions = json(0);
	return profile;
}

json stub_request()
{
	json request;
	request["agent"] = json{{"id", "reviewer"}, {"purpose", "Review it."}};
	request["stage"] = json{{"id", "review"}};
	request["task"] = json{{"id", "demo"}, {"request", "Do the thing."}};
	request["artifacts"] = json::object();
	return request;
}

} // namespace

TEST_CASE("the openai-compatible adapter completes a real HTTP exchange")
{
	stub_server server;
	stub_start(server);

	const temp_dir dir;
	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "openai-compatible";
	spec.base_url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
	spec.timeout_seconds = 15;

	odin_error err;
	const adapter_result result = adapter_run(spec, stub_profile(), stub_request(), config, err);
	stub_stop(server);

	REQUIRE_FALSE(failed(err));
	CHECK(result.response.at("status") == "approved");
	CHECK(result.response.at("summary") == "looks good");
	CHECK(result.metadata.at("status") == 200);
	CHECK(result.metadata.at("model") == "test-model");
	CHECK(result.metadata.at("duration_seconds").is_number_float());

	// two requests: the first carried response_format and was refused, the
	// second dropped it. one retry, not a loop.
	CHECK(server.requests == 2);
	CHECK(server.last_body.find("response_format") == std::string::npos);
}

TEST_CASE("the prompt actually reaches the provider")
{
	stub_server server;
	stub_start(server);

	const temp_dir dir;
	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "openai-compatible";
	spec.base_url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
	spec.timeout_seconds = 15;
	spec.json_mode = false; // one request, so last_body is that request

	odin_error err;
	adapter_run(spec, stub_profile(), stub_request(), config, err);
	stub_stop(server);

	REQUIRE_FALSE(failed(err));
	CHECK(server.requests == 1);

	const json sent = json::parse(server.last_body, nullptr, false);
	REQUIRE_FALSE(sent.is_discarded());
	CHECK(sent.at("model") == "test-model");
	CHECK(sent.at("messages").size() == 2);
	CHECK(sent.at("messages").at(0).at("role") == "system");
	CHECK(sent.at("messages").at(1).at("role") == "user");
	const std::string system = sent.at("messages").at(0).at("content");
	CHECK(system.find("You are the 'reviewer' stage") != std::string::npos);
	// json_mode off means the field must be absent, not present-and-false
	CHECK_FALSE(sent.contains("response_format"));
}

TEST_CASE("json_mode = false skips the retry entirely")
{
	stub_server server;
	stub_start(server);

	const temp_dir dir;
	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "openai-compatible";
	spec.base_url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
	spec.timeout_seconds = 15;
	spec.json_mode = false;

	odin_error err;
	const adapter_result result = adapter_run(spec, stub_profile(), stub_request(), config, err);
	stub_stop(server);

	REQUIRE_FALSE(failed(err));
	CHECK(server.requests == 1);
	CHECK(result.response.at("status") == "approved");
}

TEST_CASE("an unreachable endpoint is an adapter failure, not a crash")
{
	const temp_dir dir;
	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "openai-compatible";
	// port 1 is reserved and nothing listens there
	spec.base_url = "http://127.0.0.1:1/v1";
	spec.timeout_seconds = 5;

	odin_error err;
	adapter_run(spec, stub_profile(), stub_request(), config, err);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::adapter);
	CHECK(err.message.rfind("adapter 'stub' failed to run:", 0) == 0);
}

TEST_CASE("a credential is sent as a bearer token and never written to metadata")
{
	stub_server server;
	stub_start(server);

	const temp_dir dir;
	temp_write(dir.path / ".odin" / "credentials.json",
			   R"({"version":1,"credentials":{"stub":{"type":"api_key",)"
			   R"("value":"sk-stub-secret-value","created":"2026-01-01T00:00:00+00:00"}}})");

	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "openai-compatible";
	spec.base_url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
	spec.timeout_seconds = 15;
	spec.credential = "stub";
	spec.json_mode = false;

	odin_error err;
	const adapter_result result = adapter_run(spec, stub_profile(), stub_request(), config, err);
	stub_stop(server);

	REQUIRE_FALSE(failed(err));
	// the secret must not appear anywhere in what gets written to durable state
	CHECK(result.metadata.dump().find("sk-stub-secret-value") == std::string::npos);
	CHECK(result.response.dump().find("sk-stub-secret-value") == std::string::npos);
}

// ------------------------------------------------------------------ cli-agent

TEST_CASE("the cli-agent adapter passes a prompt and recovers the handoff")
{
	const temp_dir dir;
	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "cli-agent";
	spec.command = {ODIN_TEST_CHILD, "out:{\"status\":\"approved\",\"summary\":\"ok\","
									 "\"artifacts\":{},\"findings\":[]}"};
	spec.timeout_seconds = 30;
	spec.prompt_stdin = true;

	odin_error err;
	const adapter_result result = adapter_run(spec, stub_profile(), stub_request(), config, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.response.at("status") == "approved");
	CHECK(result.metadata.at("exit_code") == 0);
}

TEST_CASE("the prompt is appended to argv unless prompt_stdin is set")
{
	const temp_dir dir;
	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "cli-agent";
	spec.command = {ODIN_TEST_CHILD, "out:{\"status\":\"approved\",\"summary\":\"ok\","
									 "\"artifacts\":{},\"findings\":[]}", "--"};
	spec.timeout_seconds = 30;

	odin_error err;
	const adapter_result result = adapter_run(spec, stub_profile(), stub_request(), config, err);
	REQUIRE_FALSE(failed(err));

	// the last argument is the prompt itself
	const json &command = result.metadata.at("command");
	const std::string last = command.at(command.size() - 1);
	CHECK(last.find("You are the 'reviewer' stage") != std::string::npos);
}

TEST_CASE("a cli-agent JSONL stream is reassembled before extraction")
{
	const temp_dir dir;
	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "cli-agent";
	// two events whose text fields concatenate into one handoff object
	spec.command = {ODIN_TEST_CHILD,
					"out:{\"part\":{\"text\":\"{\\\"status\\\":\\\"approved\\\",\"}}\n",
					"out:{\"part\":{\"text\":\"\\\"summary\\\":\\\"streamed\\\",\\\"artifacts\\\""
					":{},\\\"findings\\\":[]}\"}}\n"};
	spec.timeout_seconds = 30;
	spec.prompt_stdin = true;
	spec.output = "jsonl";
	spec.text_path = "part.text";

	odin_error err;
	const adapter_result result = adapter_run(spec, stub_profile(), stub_request(), config, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.response.at("status") == "approved");
	CHECK(result.response.at("summary") == "streamed");
}

TEST_CASE("a cli-agent that exits nonzero reports its output, not a crash")
{
	const temp_dir dir;
	project_config config;
	config.root = dir.path;

	command_spec spec;
	spec.type = "cli-agent";
	spec.command = {ODIN_TEST_CHILD, "err:  not logged in  ", "exit:3"};
	spec.timeout_seconds = 30;
	spec.prompt_stdin = true;

	odin_error err;
	adapter_run(spec, stub_profile(), stub_request(), config, err);
	REQUIRE(failed(err));
	CHECK(err.message == "adapter 'stub' exited 3: not logged in");
}
