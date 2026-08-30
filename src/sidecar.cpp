#include "sidecar.h"

#include <cstdlib>
#include <vector>

#include "atomic_file.h"

namespace fs = std::filesystem;

constexpr std::size_t read_chunk = 4096;

std::string sidecar_default_interpreter()
{
#ifdef _WIN32
	// getenv is deprecated under msvc's secure-crt warnings; _dupenv_s is the
	// sanctioned replacement.
	char *value = nullptr;
	std::size_t size = 0;
	if (_dupenv_s(&value, &size, "ODIN_PYTHON") == 0 && value != nullptr)
	{
		std::string interpreter(value);
		std::free(value);
		if (!interpreter.empty())
			return interpreter;
	}
#else
	if (const char *value = std::getenv("ODIN_PYTHON"))
	{
		if (*value != '\0')
			return value;
	}
#endif
	return "python";
}

void sidecar_configure(sidecar &s, const fs::path &root, const std::string &interpreter)
{
	s.root = root;
	s.interpreter = interpreter.empty() ? sidecar_default_interpreter() : interpreter;
}

static bool sidecar_start(sidecar &s, odin_error &out_error)
{
	if (s.child)
		return true;

	const fs::path script = s.root / "scripts" / "contract_service.py";
	if (!fs::exists(script))
	{
		fail(out_error, error_kind::contract,
			 "contract service not found: " + file_path_utf8(script));
		return false;
	}

	s.working_directory = file_path_utf8(s.root);

	reproc::options options;
	options.working_directory = s.working_directory.c_str();
	options.redirect.in.type = reproc::redirect::pipe;
	options.redirect.out.type = reproc::redirect::pipe;
	// the child's stderr is diagnostics, not protocol. letting it through to our
	// own stderr means a python traceback is visible instead of swallowed.
	options.redirect.err.type = reproc::redirect::parent;

	const std::vector<std::string> command = {
	  s.interpreter, file_path_utf8(script)};

	auto child = std::make_unique<reproc::process>();
	const std::error_code code = child->start(command);
	if (code)
	{
		fail(out_error, error_kind::contract,
			 "could not start the contract service with '" + s.interpreter + "': " + code.message());
		return false;
	}

	s.child = std::move(child);
	s.buffered.clear();
	return true;
}

static void sidecar_discard(sidecar &s)
{
	if (!s.child)
		return;
	s.child->close(reproc::stream::in);
	s.child->wait(reproc::milliseconds(200));
	s.child->terminate();
	s.child->wait(reproc::milliseconds(200));
	s.child->kill();
	s.child.reset();
	s.buffered.clear();
}

static bool sidecar_write_all(sidecar &s, const std::string &payload)
{
	std::size_t written = 0;
	while (written < payload.size())
	{
		const auto [count, code] = s.child->write(
		  reinterpret_cast<const std::uint8_t *>(payload.data()) + written,
		  payload.size() - written);
		if (code)
			return false;
		if (count == 0)
			return false;
		written += count;
	}
	return true;
}

// pull bytes until a complete line is buffered. returns false on timeout, child
// exit, or pipe failure - all of which mean the same thing to the caller.
static bool sidecar_read_line(sidecar &s, std::string &out_line)
{
	for (;;)
	{
		const std::size_t newline = s.buffered.find('\n');
		if (newline != std::string::npos)
		{
			out_line = s.buffered.substr(0, newline);
			s.buffered.erase(0, newline + 1);
			if (!out_line.empty() && out_line.back() == '\r')
				out_line.pop_back();
			return true;
		}

		const auto [events, poll_code] = s.child->poll(
		  reproc::event::out | reproc::event::exit,
		  reproc::milliseconds(sidecar_timeout_seconds * 1000));
		if (poll_code || events == 0)
			return false;
		if ((events & reproc::event::out) == 0)
			return false; // exited

		std::uint8_t chunk[read_chunk];
		const auto [count, read_code] = s.child->read(reproc::stream::out, chunk, sizeof(chunk));
		if (read_code || count == 0)
			return false;
		s.buffered.append(reinterpret_cast<const char *>(chunk), count);
	}
}

// one attempt at a full round trip. the caller decides whether to restart.
static bool sidecar_exchange(sidecar &s, const json &request, json &out_reply)
{
	// ensure_ascii keeps the payload free of literal newlines, which is what
	// makes line framing safe without escaping or length prefixes.
	const std::string payload = request.dump(-1, ' ', true) + "\n";
	if (!sidecar_write_all(s, payload))
		return false;

	const int identifier = request.at("id").get<int>();
	for (;;)
	{
		std::string line;
		if (!sidecar_read_line(s, line))
			return false;

		json reply = json::parse(line, nullptr, false);
		if (reply.is_discarded() || !reply.is_object())
			return false;

		// a stale reply from a restarted exchange would desynchronise the
		// stream, so skip anything that is not the answer we are waiting for.
		const auto id = reply.find("id");
		if (id == reply.end() || !id->is_number_integer())
			continue;
		if (id->get<int>() != identifier)
			continue;

		out_reply = std::move(reply);
		return true;
	}
}

json sidecar_call(sidecar &s, json request, odin_error &out_error)
{
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		if (!sidecar_start(s, out_error))
			return json::object();

		request["id"] = s.next_request_id++;

		json reply;
		if (sidecar_exchange(s, request, reply))
			return reply;

		sidecar_discard(s);
		if (s.restarts > 0)
			break;
		++s.restarts;
	}

	fail(out_error, error_kind::contract,
		 "the contract service stopped responding; see stderr above for its output");
	return json::object();
}

void sidecar_stop(sidecar &s)
{
	if (!s.child)
		return;

	json shutdown;
	shutdown["id"] = s.next_request_id++;
	shutdown["op"] = "shutdown";
	sidecar_write_all(s, shutdown.dump(-1, ' ', true) + "\n");

	s.child->close(reproc::stream::in);
	s.child->wait(reproc::milliseconds(1000));
	s.child->terminate();
	s.child->wait(reproc::milliseconds(500));
	s.child->kill();
	s.child.reset();
	s.buffered.clear();
}
