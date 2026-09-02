#include "auth_command.h"

#include <cstdio>
#include <iostream>
#include <string>

#include "atomic_file.h"
#include "credentials.h"
#include "json_io.h"
#include "secure_input.h"

namespace fs = std::filesystem;

namespace
{

int report(const odin_error &error)
{
	std::cerr << "odin: " << error.message << "\n";
	return 2;
}

// pad to a column width by character count. printf's %-24s pads by bytes, which
// misaligns the moment a credential name or note contains anything non-ASCII.
std::string pad(const std::string &text, std::size_t width)
{
	std::size_t characters = 0;
	for (const char byte : text)
	{
		if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80)
			++characters;
	}
	return characters >= width ? text : text + std::string(width - characters, ' ');
}

std::string field_or(const json &entry, const char *key, const std::string &fallback)
{
	const auto found = entry.find(key);
	return found != entry.end() && found->is_string() ? found->get<std::string>() : fallback;
}

// `~` and `~user` are shell conventions; a path handed to --from-file has not
// been through a shell when it comes from a script or a GUI.
fs::path expand_user(const std::string &text)
{
	if (text.empty() || text[0] != '~')
		return fs::path(text);
	if (text.size() > 1 && text[1] != '/' && text[1] != '\\')
		return fs::path(text); // ~other-user is not resolved; leave it alone

#ifdef _WIN32
	char *home = nullptr;
	std::size_t size = 0;
	if (_dupenv_s(&home, &size, "USERPROFILE") != 0 || home == nullptr)
		return fs::path(text);
	fs::path resolved(home);
	std::free(home);
#else
	const char *home = std::getenv("HOME");
	if (home == nullptr)
		return fs::path(text);
	fs::path resolved(home);
#endif
	if (text.size() > 2)
		resolved /= fs::path(text.substr(2));
	return resolved;
}

int auth_set(credential_store &store, const auth_options &options)
{
	std::string secret;
	if (options.read_stdin)
	{
		std::string all((std::istreambuf_iterator<char>(std::cin)),
						std::istreambuf_iterator<char>());
		const std::size_t first = all.find_first_not_of(" \t\n\r\f\v");
		const std::size_t last = all.find_last_not_of(" \t\n\r\f\v");
		secret = first == std::string::npos ? "" : all.substr(first, last - first + 1);
	}
	else if (!options.value.empty())
	{
		// documented but discouraged: an argv secret is visible in process
		// listings to any other process of the same user
		secret = options.value;
	}
	else
	{
		bool echoed = false;
		if (!secure_input_read("Secret for '" + options.name + "' (not echoed): ", secret, echoed))
		{
			std::cerr << "odin: no secret was provided\n";
			return 2;
		}
		if (echoed)
			std::cerr << "warning: this terminal could not disable echo; the secret was visible\n";
	}

	odin_error err;
	if (!credential_set_api_key(store, options.name, secret, json(options.note.empty()
																	? json(nullptr)
																	: json(options.note)),
								err))
		return report(err);

	json described;
	if (!credential_describe(store, options.name, described, err))
		return failed(err) ? report(err) : 2;

	std::cout << "stored '" << options.name << "' ("
			  << described.at("value").get<std::string>() << ") in "
			  << file_path_utf8(credential_store_path(store)) << "\n";
	std::cout << "Reference it from an adapter with: --credential " << options.name << "\n";
	return 0;
}

int auth_list(const credential_store &store, const auth_options &options)
{
	odin_error err;
	const json entries = credential_describe_all(store, err);
	if (failed(err))
		return report(err);

	if (options.as_json)
	{
		json payload;
		payload["credentials"] = entries;
		payload["store"] = file_path_utf8(credential_store_path(store));
		std::cout << payload.dump(2, ' ', true, json::error_handler_t::replace) << "\n";
		return 0;
	}

	if (entries.empty())
	{
		std::cout << "No credentials stored.\n";
		std::cout << "Add one with: odin auth set <name>\n";
		return 1; // "nothing found" is exit 1, not a failure
	}

	for (const json &entry : entries)
	{
		std::string flags;
		const auto expired = entry.find("expired");
		if (expired != entry.end() && expired->is_boolean())
			flags = expired->get<bool>() ? "  [EXPIRED]" : "  [valid]";

		const std::string note = field_or(entry, "note", "");
		std::cout << pad(entry.at("name").get<std::string>(), 24) << " "
				  << pad(entry.at("type").get<std::string>(), 8) << " "
				  << pad(entry.at("value").get<std::string>(), 16) << flags
				  << (note.empty() ? "" : "  " + note) << "\n";
	}
	std::cout << "\nstore: " << file_path_utf8(credential_store_path(store)) << "\n";
	return 0;
}

int auth_remove(credential_store &store, const auth_options &options)
{
	odin_error err;
	const bool removed = credential_remove(store, options.name, err);
	if (failed(err))
		return report(err);
	if (removed)
	{
		std::cout << "removed '" << options.name << "'\n";
		return 0;
	}
	std::cerr << "no credential named '" << options.name << "'\n";
	return 1;
}

// Import a token another tool already obtained, rather than making the user log
// in a second time. The source file belongs to that tool, so the shape is
// discovered rather than assumed.
int auth_import(credential_store &store, const auth_options &options)
{
	const fs::path source = expand_user(options.from_file);

	odin_error err;
	const json payload = json_read(source, err);
	if (failed(err))
		return report(err);

	const auto entry = payload.find(options.provider);
	if (entry == payload.end() || !entry->is_object())
	{
		std::string available;
		for (const auto &[key, value] : payload.items())
		{
			if (!value.is_object())
				continue;
			if (!available.empty())
				available += ", ";
			available += key;
		}
		std::cerr << "odin: provider '" << options.provider << "' not found in "
				  << file_path_utf8(source) << ". Available: " << available << "\n";
		return 2;
	}

	// three spellings are common across tools; take whichever is present
	std::string access;
	for (const char *key : {"access", "value", "token"})
	{
		const auto found = entry->find(key);
		if (found != entry->end() && found->is_string() && !found->get<std::string>().empty())
		{
			access = found->get<std::string>();
			break;
		}
	}
	if (access.empty())
	{
		std::cerr << "odin: '" << options.provider << "' in " << file_path_utf8(source)
				  << " has no access token field\n";
		return 2;
	}

	const std::string name = options.name.empty() ? options.provider : options.name;
	const json note = "imported from " + source.filename().string();

	const json refresh = entry->value("refresh", json(nullptr));
	const json expires = entry->value("expires", json(nullptr));

	// an entry carrying refresh/expires is an OAuth token and keeps those
	// fields, so expiry can be reported rather than discovered as a 401
	const bool is_oauth = !refresh.is_null() || !expires.is_null();
	const bool stored = is_oauth
						  ? credential_set_oauth(store, name, access, refresh, expires, note, err)
						  : credential_set_api_key(store, name, access, note, err);
	if (!stored)
		return report(err);

	json described;
	if (!credential_describe(store, name, described, err))
		return failed(err) ? report(err) : 2;

	std::cout << "imported '" << options.provider << "' as '" << name << "' ("
			  << described.at("value").get<std::string>() << ")\n";

	const auto expired = described.find("expired");
	if (expired != described.end() && expired->is_boolean() && expired->get<bool>())
		std::cerr << "warning: the imported token is already expired\n";
	return 0;
}

} // namespace

int auth_command_run(const fs::path &project_root, const auth_options &options)
{
	credential_store store;
	credential_store_configure(store, project_root);

	if (options.subcommand == "set")
		return auth_set(store, options);
	if (options.subcommand == "list")
		return auth_list(store, options);
	if (options.subcommand == "remove")
		return auth_remove(store, options);
	if (options.subcommand == "import")
		return auth_import(store, options);

	std::cerr << "odin: unknown auth subcommand '" << options.subcommand << "'\n";
	return 2;
}
